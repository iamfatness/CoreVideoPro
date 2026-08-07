/**
 * The show engine — the class that owns every module instance and is the
 * first real composition in this package. Twenty-one modules were shipped
 * individually tested but never wired together; this is that wiring.
 *
 * This file covers construction, restore-from-disk, the snapshot accessor,
 * roster intake, the seating tick (Task 5), the active-speaker dispatch gate
 * (Task 6), and the derived layers (Task 7): look resolution against the
 * live roster and hands queue, paging, manual box assignment, program/
 * preview transport (with degradation for a host with no preview bus), and
 * the overlay director. Host command emission (diffing derived state into
 * `HostAdapter` calls for slots/looks/gallery/nameplates/questions) and the
 * polling loop are Task 8/9+.
 *
 * The one rule every reviewer of this file must re-verify: `clampPage` and
 * `resolveLook` are called together, exactly once per tick, and always
 * share the SAME resolved `handsQueue` capability. Both default an omitted
 * capability to unusable, so passing it to one and not the other produces
 * no crash and no error — just an operator paging control that silently
 * does nothing. See the call site inside `tick()` for the full story.
 */

import type { Clock } from "./clock.js";
import type { HostAdapter } from "./hostAdapter.js";
import type { PositionAssigner } from "./speakerRecency.js";
import { FiloAssigner } from "./speakerRecency.js";
import { shouldFollowSpeaker } from "./speakerGate.js";
import type { ShowEngineConfig } from "./config.js";
import type { MukanaClient, MukanaEndpoint, MukanaHealth } from "./mukanaClient.js";
import { MukanaRegistry, type MukanaOutcome } from "./mukanaParse.js";
import { LiveSlots } from "./liveSlots.js";
import { GalleryDirector } from "./galleryDirector.js";
import { OverrideDb, type OverrideRecord } from "./overrideDb.js";
import { ZoomIngest, type ZoomEvent } from "./zoomIngest.js";
import { ProgramBus } from "./programBus.js";
import { OverlayDirector } from "./overlayDirector.js";
import { buildPanelistDb } from "./panelistDb.js";
import { deriveTally } from "./tallyPublisher.js";
import { resolveCapabilities } from "./capabilities.js";
import { buildSnapshot, type ShowSnapshot } from "./showSnapshot.js";
import { StateStore, STATE_VERSION, type PersistedShowState } from "./persistence.js";
import {
  clampPage,
  effectiveBoxFill,
  findChairSlots,
  resolveLook,
  type LookResolution,
  type ManualBoxAssignments
} from "./lookDirector.js";
import { stripChairs } from "./handsQueue.js";
import {
  EXCLUSIVE_ROLES,
  type LookDefinition,
  type MukanaQuestion,
  type Panelist,
  type ProgramSource,
  type QueueState,
  type Role,
  type Slot
} from "./contracts.js";
import type { PersonKey } from "./personKey.js";

/**
 * Minimum time between persisted saves, enforced against the injected
 * `Clock` rather than a wall-clock timer — a live show never stops calling
 * `tick()`, and writing the full state document on every single tick would
 * mean a disk write per frame. The first save after construction (or after
 * the previous save) is unthrottled; every one after that must wait this
 * long since the last actual write.
 */
const SAVE_DEBOUNCE_MS = 1000;

/** True for the two roles `OverrideDb.assignExclusiveRole` knows how to enforce. */
function isExclusiveOverrideRole(role: Role): role is "host" | "reader" {
  return (EXCLUSIVE_ROLES as readonly Role[]).includes(role);
}

function participantIdSet(participants: readonly { participantId: string }[]): Set<string> {
  return new Set(participants.map((p) => p.participantId));
}

function sameIdSet(a: ReadonlySet<string>, b: ReadonlySet<string>): boolean {
  if (a.size !== b.size) return false;
  for (const id of a) {
    if (!b.has(id)) return false;
  }
  return true;
}

/** The PIN of whoever is seated at `slotNumber`, or `null` for an empty/absent slot. */
function pinAtSlot(slots: readonly Slot[], slotNumber: number | null): string | null {
  if (slotNumber === null) return null;
  const entry = slots.find((candidate) => candidate.slot === slotNumber);
  return entry?.panelist?.pin ?? null;
}

export type ShowEngineDeps = {
  config: ShowEngineConfig;
  host: HostAdapter;
  clock: Clock;
  store: StateStore;
  mukana?: MukanaClient; // absent for a show with config.mukana === null
  assigner?: PositionAssigner; // defaults to FiloAssigner sized to config.capacity
};

/**
 * Health reported for a show with no Mukana registry at all: every endpoint
 * reads as failing with a fixed, honest detail rather than `null`/`"ok"`,
 * so `resolveCapabilities` has real health data to resolve against (it maps
 * this, combined with a registry-less config's all-off `integrations`, to
 * `disabled` for every capability — see `capabilities.ts`).
 */
const NO_REGISTRY_HEALTH: Record<MukanaEndpoint, MukanaHealth> = {
  panelists: { state: "failing", consecutiveFailures: 0, detail: "no registry configured" },
  hands: { state: "failing", consecutiveFailures: 0, detail: "no registry configured" },
  question: { state: "failing", consecutiveFailures: 0, detail: "no registry configured" }
};

/**
 * A fresh, empty `QueueState`. Called at each field-init (and will be
 * called again by a future `reset()`), never assigned as a shared
 * module-level constant — `previous`/`upcoming` are arrays, and a shared
 * reference would let an in-place mutation on one `ShowEngine` instance
 * (a `.push` from Task 9's hands-queue wiring, say) leak into every other
 * instance that read the same default, torn-down ones included. Mirrors
 * `overlayDirector.ts`'s `emptyState()`.
 */
function emptyQueue(): QueueState {
  return { previous: [], current: null, upcoming: [] };
}

export class ShowEngine {
  private readonly config: ShowEngineConfig;
  private readonly host: HostAdapter;
  private readonly clock: Clock;
  private readonly store: StateStore;
  private readonly mukanaClient: MukanaClient | undefined;
  private readonly assigner: PositionAssigner;
  private readonly galleryCellCount: number;

  private readonly mukanaRegistry: MukanaRegistry;
  private readonly overrideDb: OverrideDb;
  private readonly zoomIngest: ZoomIngest;
  private readonly programBus: ProgramBus;
  private readonly overlayDirector: OverlayDirector;

  private liveSlots: LiveSlots;
  private gallery: GalleryDirector;

  private tickRevision = 0;
  private selectedLookId: string | null = null;
  private currentLook: LookResolution | null = null;
  private page = 0;
  private manualBoxes: ManualBoxAssignments = {};
  private queue: QueueState = emptyQueue();
  private unseatedPanelists: Panelist[] = [];

  /**
   * The current audience question, or `null` when there is none to show.
   * No `ShowEngine` input sets this yet — Mukana question-feed wiring is a
   * later task — so it is always `null` today; `OverlayDirector.update`
   * still takes it every tick so that wiring is a one-line change when it
   * lands, not a new call shape.
   */
  private question: MukanaQuestion | null = null;

  /** Whether the operator currently wants the audience question on screen. Set by `setQuestionVisible`. */
  private questionVisible = false;

  /**
   * Why the last `nextGuest`/`prevGuest` call did not move the page, or
   * `null` when the last paging attempt (if any) succeeded. Only
   * `nextGuest`/`prevGuest` write this — `tick()`'s own `clampPage` step
   * never touches it, so a refusal recorded between ticks is still visible
   * on the snapshot the following tick publishes.
   */
  private pagingRefused: string | null = null;

  /**
   * Whether `overlayDirector.update()` reported a change on the most recent
   * tick. Task 8's host-emission step needs this to decide whether the
   * overlay layer is worth re-sending; Task 7 only has to preserve it.
   */
  private overlaysChanged = false;

  /**
   * The participant id set the roster was last seated against, so `tick()`
   * can tell a genuine roster change (seat what's new, surface overflow)
   * from an in-place update to someone already seated (hold seats still).
   * `null` before the first seating ever runs, which always counts as
   * "changed" — there is no prior arrangement to hold.
   */
  private lastSeatedParticipantIds: ReadonlySet<string> | null = null;

  /**
   * Set by any non-roster input that can change what gets seated
   * (`setOverride`, `clearOverride`, a real Mukana payload) or persisted
   * (`setLook`). Consumed at the START of every `tick()` regardless of
   * whether that tick ends up saving — this is a "did something happen
   * since the seat step last ran" flag, not a "there is unsaved state"
   * flag, so it must never linger across a debounce-skipped save (that
   * would force a spurious reseat on every later tick until a save
   * finally lands). `pendingPersist` below is the flag that lingers.
   */
  private otherInputsChanged = false;

  /**
   * Set whenever an input that affects persisted state changes (roster
   * commit, `setOverride`, `clearOverride`, `setLook`, a real Mukana
   * payload). Cleared only when `tick()` actually writes — NOT merely when
   * enough time has passed — so a change that arrives mid-debounce-window
   * is never lost, just delayed to the next tick that clears the window.
   */
  private pendingPersist = false;

  /** Wall-clock time (per the injected `Clock`) of the last actual save, or `null` before the first one. */
  private lastSaveTime: number | null = null;

  /**
   * The most recent `onActiveSpeaker` id since the last tick consumed one,
   * or `null` if none arrived. A single slot, not a queue — several events
   * within one tick collapse to the latest, matching how a live meeting's
   * active-speaker signal actually behaves (it names who is CURRENTLY
   * talking, not a history). Consumed and cleared at the top of the gate
   * step in `tick()` every tick, whether or not it holds anything, so a
   * pending speaker is never replayed into a later tick.
   */
  private pendingSpeakerId: string | null = null;

  constructor(deps: ShowEngineDeps) {
    if (deps.mukana !== undefined && deps.config.mukana === null) {
      throw new Error(
        "ShowEngine: a mukana client was provided but config.mukana is null (no address configured) — " +
          "either pass a config with a mukana address, or omit the mukana client"
      );
    }

    this.config = deps.config;
    this.host = deps.host;
    this.clock = deps.clock;
    this.store = deps.store;
    this.mukanaClient = deps.mukana;
    this.assigner = deps.assigner ?? new FiloAssigner({ capacity: deps.config.capacity });

    this.galleryCellCount = Math.min(deps.config.galleryCells, deps.host.capabilities().maxGalleryCells);

    this.mukanaRegistry = new MukanaRegistry();
    this.overrideDb = new OverrideDb();
    this.zoomIngest = new ZoomIngest();
    this.programBus = new ProgramBus({ skipRoles: deps.config.skipRoles });
    this.overlayDirector = new OverlayDirector();

    this.liveSlots = new LiveSlots({
      capacity: deps.config.capacity,
      utilityPinBase: deps.config.utilityPinBase
    });
    this.gallery = new GalleryDirector({ cells: this.galleryCellCount });
  }

  /**
   * Select the active look by id. Throws on an unknown look id. Switching
   * to a *different* look clears `manualBoxes` entirely (spec §3.2) — box 1
   * of one arrangement is not box 1 of another, so carrying manual
   * assignments across a look change would put the wrong person in the
   * wrong window. Re-selecting the SAME look is a no-op on `manualBoxes`,
   * so an idempotent re-select never wipes the operator's work. The actual
   * resolve-against-the-roster/clamp-the-page work happens in `tick()`.
   */
  setLook(lookId: string): void {
    const look = this.config.looks.find((candidate) => candidate.id === lookId);
    if (look === undefined) {
      throw new Error(`ShowEngine.setLook: unknown look id ${JSON.stringify(lookId)}`);
    }
    if (this.selectedLookId !== lookId) {
      this.manualBoxes = {};
    }
    this.selectedLookId = lookId;
    this.pendingPersist = true;
    this.otherInputsChanged = true;
  }

  /**
   * Test-only escape hatch: writes the pending page directly, with **no**
   * clamping. Production code should never call this — operator paging
   * goes through `nextGuest`/`prevGuest`, and `tick()`'s own `clampPage`
   * step is what keeps `this.page` sane between ticks, deriving the valid
   * range from the current capability state and queue every time. This
   * exists so a property test can plant a page the way a *stale* one would
   * actually arrive at the top of a tick — e.g. left over from a
   * fill-strategy flip or a look change — and prove `tick()` survives it
   * rather than throwing.
   */
  setPage(page: number): void {
    this.page = page;
  }

  /**
   * Move the paging window forward/back by one. Only takes effect when the
   * active look's boxes are actually filling from the hands queue this
   * tick (`effectiveBoxFill(...) === "queue"`) — under manual fill (a
   * manual look, or a queue look whose hands feed just died) there is no
   * queue window to move through. In that case the call is inert and
   * `pagingRefused` records why, rather than throwing or silently doing
   * nothing (spec §4). `tick()`'s own `clampPage` step is what keeps
   * whatever page results valid; this method only records operator intent.
   */
  nextGuest(): void {
    this.adjustPage(1);
  }

  /** The mirror of `nextGuest`; see its docs. */
  prevGuest(): void {
    this.adjustPage(-1);
  }

  private adjustPage(delta: number): void {
    const look = this.lookById(this.selectedLookId);
    if (look === null) {
      this.pagingRefused = "paging refused: no look is selected";
      return;
    }

    const caps = resolveCapabilities(this.config, this.mukanaHealth());
    const fill = effectiveBoxFill(look, caps.handsQueue);
    if (fill !== "queue") {
      this.pagingRefused = `paging refused: box fill is ${fill}, not queue-driven`;
      return;
    }

    this.pagingRefused = null;
    this.page += delta;
  }

  /**
   * Write or overwrite one manual box assignment. Meaningful only under
   * manual box fill (`resolveLook` simply ignores manual assignments for a
   * look currently filling from the queue), but recorded unconditionally —
   * the operator may be setting it up in advance of a fill-strategy switch.
   */
  assignBox(box: number, slot: number): void {
    this.manualBoxes = { ...this.manualBoxes, [box]: slot };
    this.pendingPersist = true;
  }

  /** Remove one manual box assignment, leaving the rest untouched. */
  clearBox(box: number): void {
    const next = { ...this.manualBoxes };
    delete next[box];
    this.manualBoxes = next;
    this.pendingPersist = true;
  }

  /** Toggle whether the audience question overlay should render. Forwarded to `OverlayDirector.update` every tick. */
  setQuestionVisible(on: boolean): void {
    this.questionVisible = on;
  }

  /**
   * Stage a source in preview. Always updates `ProgramBus`'s own preview
   * field — that is just in-memory bus state, not a host emission — but
   * only forwards to the host when `host.capabilities().hasPreviewBus` is
   * true. A host with no preview bus has no use for a staged-but-unseen
   * source, and must never receive this call (transport degradation rule).
   */
  setPreview(source: ProgramSource): void {
    this.programBus.setPreview(source);
    if (this.host.capabilities().hasPreviewBus) {
      this.host.setPreview(source);
    }
  }

  /**
   * `cut()` and `auto()` are host-facing twins of `ProgramBus.cut`/`auto`,
   * modeled the same way here: identical except for which host method they
   * call. On a host with a preview bus, both swap `ProgramBus`'s
   * program/preview and tell the host to do the same. On a host with none,
   * there is nothing to swap TO except whatever is currently staged in
   * `ProgramBus`'s preview field, so both route to `directCut` with that
   * value instead — and never call `host.cut()`/`host.auto()`, since this
   * host declared it has no such concept.
   */
  cut(): void {
    if (this.host.capabilities().hasPreviewBus) {
      this.programBus.cut();
      this.host.cut();
    } else {
      this.directCut(this.programBus.state().preview);
    }
  }

  /** See `cut()` — identical transport-degradation shape, differing only in which host method it calls. */
  auto(transitionId?: string): void {
    if (this.host.capabilities().hasPreviewBus) {
      this.programBus.auto();
      this.host.auto(transitionId);
    } else {
      this.directCut(this.programBus.state().preview);
    }
  }

  /**
   * Cut program straight to `source`, bypassing preview. There is no
   * `HostAdapter.directCut` — a direct cut has no host-transport
   * equivalent of its own (Task 8's derived-state emission is what tells a
   * connected host what is now on screen), so this only ever touches the
   * in-process `ProgramBus`.
   */
  directCut(source: ProgramSource): void {
    this.programBus.directCut(source);
  }

  /**
   * Record a host roster event. This does NO seating — it only forwards to
   * `ZoomIngest.apply`, which buffers into a working set behind a publish
   * gate. Host events arrive at frame rate; seating happens once per tick,
   * the same discipline the CVP shell learned the hard way with
   * `RefreshSurfaceBindings` (rebuilding a bound collection at event rate
   * instead of a coalesced tick rate is what produces the CoreMessagingXP
   * fail-fast class documented in this repo's CLAUDE.md).
   */
  onZoomEvent(event: ZoomEvent): void {
    this.zoomIngest.apply(event);
  }

  /**
   * Apply one Mukana panelists fetch outcome. Only a `"data"` outcome
   * carries anything to merge — `dormant` (off-hours) and `invalid`
   * (transport failure) outcomes are health information the `MukanaClient`
   * that produced this outcome already recorded for itself; there is
   * nothing here for the registry to apply.
   */
  onMukanaPanelists(outcome: MukanaOutcome): void {
    if (outcome.kind !== "data") return;
    this.mukanaRegistry.merge(outcome.db);
    this.pendingPersist = true;
    this.otherInputsChanged = true;
  }

  /**
   * Write an operator role override. `OverrideDb.set` alone would happily
   * leave two hosts if `record.role` names an exclusive role someone else
   * already holds — `assignExclusiveRole` is the only thing that demotes a
   * prior holder, so it runs right after `set` whenever the written role is
   * exclusive. A registry-less show passes an empty registry, which is
   * `assignExclusiveRole`'s documented "enforce across the override table
   * alone" case.
   */
  setOverride(record: OverrideRecord): void {
    this.overrideDb.set(record);
    if (isExclusiveOverrideRole(record.role)) {
      this.overrideDb.assignExclusiveRole(record.personKey, record.role, this.mukanaRegistry.current());
    }
    this.pendingPersist = true;
    this.otherInputsChanged = true;
  }

  /** Remove an operator role override, reverting that person to whatever Mukana (or nothing) declares. */
  clearOverride(personKey: PersonKey): void {
    this.overrideDb.delete(personKey);
    this.pendingPersist = true;
    this.otherInputsChanged = true;
  }

  /**
   * Record a host active-speaker event. This does NOTHING beyond recording
   * `participantId` as the pending speaker — no role lookup, no dispatch to
   * the assigner or `ProgramBus`. That happens in `tick()`, after the
   * panelist database has been rebuilt for this tick, so the gate always
   * sees current editorial roles rather than a stale or (at startup) empty
   * one. See the gate step in `tick()` for why this can't run here.
   */
  onActiveSpeaker(participantId: string): void {
    this.pendingSpeakerId = participantId;
  }

  /** Toggle whether `ProgramBus` cuts program to the active speaker. Forwarded directly; no gating of its own. */
  setActiveSpeakerFollow(on: boolean): void {
    this.programBus.setActiveSpeakerFollow(on);
  }

  /**
   * Load persisted state through the `StateStore` and apply it. Returns
   * `false` when there was nothing to load (no file, corrupt file, foreign
   * version — `StateStore.load` already treats all of those as "no state").
   * A `LiveSlotsRestoreError` or `GalleryError` from a structurally-bad file
   * that passed the store's shallow check is a real corruption and is left
   * to propagate rather than silently downgrading to an empty roster.
   */
  async restore(): Promise<boolean> {
    const persisted = await this.store.load();
    if (persisted === null) return false;

    const restoredSlots = LiveSlots.fromJSON(persisted.slots, {
      capacity: this.config.capacity,
      utilityPinBase: this.config.utilityPinBase
    });
    const restoredGallery = GalleryDirector.fromJSON(persisted.gallery, {
      cells: this.galleryCellCount
    });

    this.liveSlots = restoredSlots;
    this.gallery = restoredGallery;
    this.overrideDb.restore(persisted.overrides);

    // Seed the seat step's "last seated against" id set from what was just
    // restored, so that IF Zoom's first post-restart commit reports exactly
    // the same participant ids (a brief blip rather than a real restart of
    // the meeting), the seat step correctly takes the `refresh` path and
    // holds those seats in place rather than an unnecessary `rebuild` —
    // today that happens to reproduce identical positions (deterministic
    // sort, no manual seat-move feature yet), but this is what keeps it
    // correct once one exists. Belt-and-suspenders with the revision-0
    // guard in `tick()`, which is what actually stops a wipe before Zoom
    // has said anything at all.
    this.lastSeatedParticipantIds = new Set(
      restoredSlots
        .slots()
        .flatMap((slot) => (slot.panelist === null ? [] : [slot.panelist.participantId]))
    );

    // A restored assignment set belongs to whatever look was selected when
    // it was saved. Applying it under a different look would put whoever
    // the operator put in box 1 of one arrangement into box 1 of another,
    // which is not the same seat — discard rather than inherit.
    this.manualBoxes =
      persisted.lookId === this.selectedLookId ? { ...persisted.manualBoxes } : {};

    return true;
  }

  /**
   * The heartbeat. Runs in a fixed order: commit the buffered roster, seat
   * it (holding seats still for an unchanged participant set, reseating
   * only when it actually changed), dispatch a pending active speaker
   * (Task 6), recompute the derived layers — look resolution, paging,
   * program staging, overlays (Task 7) — persist if the debounce window
   * allows it, emit host commands (Task 8 — a no-op today), then advance
   * the revision and publish.
   *
   * Step 1 alone (`ZoomIngest.commit()`) can say nothing changed, but the
   * tick still runs to completion — a look or capability change can alter
   * output against a static roster, and the revision always advances so a
   * consumer polling `revision()` can tell every tick apart.
   */
  async tick(): Promise<ShowSnapshot> {
    const rosterCommitted = this.zoomIngest.commit();
    const otherChanged = this.otherInputsChanged;
    this.otherInputsChanged = false;

    // A non-roster input (setLook/setOverride/onMukanaPanelists) alone must
    // never run the seat step while ZoomIngest has never committed real
    // data (revision 0) — right after a fresh `restore()`, ZoomIngest is
    // still empty because restore() does not (and cannot) repopulate it.
    // Without this guard, `setLook` between `restore()` and Zoom
    // reconnecting would seat/rebuild against an empty roster, clearing
    // every restored seat and persisting the wipe on the next debounced
    // save. A real roster commit always proceeds regardless — that IS live
    // data arriving.
    const rosterInputsChanged = rosterCommitted || (otherChanged && this.zoomIngest.revision > 0);

    if (rosterInputsChanged) {
      const participants = this.zoomIngest.snapshot();
      const panelists = buildPanelistDb(
        participants,
        this.mukanaRegistry.current(),
        this.overrideDb.entries()
      );
      const currentIds = participantIdSet(participants);

      if (
        this.lastSeatedParticipantIds !== null &&
        sameIdSet(currentIds, this.lastSeatedParticipantIds)
      ) {
        // Same people, at most changed properties (video/audio/hand/role) —
        // hold every seat still. A guest toggling their camera must not
        // reseat the room. A Zoom departure ("left") does NOT change the id
        // set either (ZoomIngest keeps a departed participant, marked
        // offline, "so they can be restored on reconnect" — zoomIngest.ts)
        // and `refresh` itself keeps a since-vanished seat's panelist and
        // marks it offline rather than clearing it (liveSlots.ts) — a
        // connection blip must never drop a panelist off air, and a
        // reconnect must return them to the SAME slot. Owner ruling,
        // 2026-08-06: clearing a seat is an explicit operator action, never
        // an automatic consequence of an offline flag.
        this.liveSlots.refresh(panelists);
      } else {
        // The roster itself changed (a join, a departure via a full roster
        // resync that omits someone, or the very first tick, when there is
        // no prior arrangement to hold). Seat deterministically by
        // participant id — sorting by name would reshuffle the room
        // whenever someone renamed themselves mid-show, and this sort must
        // stay independent of `ZoomIngest.commit()`'s own internal sort
        // (zoomIngest.ts) rather than rely on it: the two use different
        // collation (code-unit vs `localeCompare`), and this module must
        // not depend on another module's undocumented internal ordering —
        // and never drop whoever doesn't fit: `rebuild`'s overflow return
        // is what the published snapshot's `unseated` reports.
        const sorted = [...panelists.values()].sort((a, b) =>
          a.participantId < b.participantId ? -1 : a.participantId > b.participantId ? 1 : 0
        );
        this.unseatedPanelists = this.liveSlots.rebuild(sorted);
      }
      this.lastSeatedParticipantIds = currentIds;

      // Reconcile the unseated list against this tick's fresh panelist data
      // even on a `refresh`-path tick: `rebuild` above already recomputes
      // `unseatedPanelists` from-scratch, but a `refresh`-path tick never
      // touches it at all, so an overflow panelist's own property changes
      // (video/audio/role) would otherwise be invisible on the published
      // snapshot until the next roster-changing rebuild. Safe because a
      // `refresh`-path tick only runs when the id set is unchanged, so
      // every previously-unseated id is still present in `panelists`.
      this.unseatedPanelists = this.unseatedPanelists.map(
        (panelist) => panelists.get(panelist.participantId) ?? panelist
      );

      this.pendingPersist = true;
    }

    // The active-speaker dispatch gate (Task 6). Take the pending speaker
    // id and clear it FIRST, unconditionally, so a slot that held nothing
    // this tick can never replay into a later one. The panelist database is
    // rebuilt fresh here (mirrors `snapshot()`'s own rebuild) so role
    // resolution always reflects the CURRENT roster + overrides, including
    // one the operator assigned seconds earlier — never the one-tick-stale
    // view intake would see, and never the empty view a fresh restore would
    // see. `shouldFollowSpeaker` runs BEFORE anything else touches the id:
    // before the assigner, before `ProgramBus`. The real (possibly `null`)
    // role is passed through to BOTH downstream calls — fixed in fix round
    // 1 (was `role ?? "panelist"` on the `ProgramBus` call only). Fabricating
    // `"panelist"` for an unrostered speaker was a lie `ProgramBus`'s own
    // internal `shouldFollowSpeaker` could then act on: with
    // `skipRoles: ["panelist"]`, the engine's gate correctly let a `null`
    // role through (unconditional `true`), but the fabricated `"panelist"`
    // handed to `ProgramBus.onActiveSpeaker` made ITS internal gate veto —
    // the assigner moved a pool position that program never cut to. Passing
    // the real role keeps the engine's gate and `ProgramBus`'s internal gate
    // evaluating the identical input, so they can never disagree.
    const pendingSpeaker = this.pendingSpeakerId;
    this.pendingSpeakerId = null;
    if (pendingSpeaker !== null) {
      const speakerPanelists = buildPanelistDb(
        this.zoomIngest.snapshot(),
        this.mukanaRegistry.current(),
        this.overrideDb.entries()
      );
      const role = speakerPanelists.get(pendingSpeaker)?.role ?? null;
      if (shouldFollowSpeaker(role, this.config.skipRoles)) {
        this.assigner.onActiveSpeaker(pendingSpeaker);
        this.programBus.onActiveSpeaker(pendingSpeaker, role);
      }
    }

    // The derived layers (Task 7). Resolve capabilities ONCE for this tick
    // and thread that single value everywhere — most importantly into the
    // `clampPage`/`resolveLook` pair immediately below, which MUST always
    // receive the same `handsQueue` capability. Each defaults an omitted
    // capability to unusable, so passing it to one call and not the other
    // silently pins the operator to page 0 (no crash, no error — see the
    // file-level Plan 4 obligation this class inherited). Never introduce a
    // third call site that invokes only one of the two.
    const caps = resolveCapabilities(this.config, this.mukanaHealth());

    const slots = this.liveSlots.slots();
    const { hostSlot: seatedHostSlot, readerSlot: seatedReaderSlot } = findChairSlots(slots);
    const strippedQueue = stripChairs(this.queue, {
      hostPin: pinAtSlot(slots, seatedHostSlot),
      readerPin: pinAtSlot(slots, seatedReaderSlot)
    });

    const look = this.lookById(this.selectedLookId);
    const previousLookId = this.currentLook?.lookId ?? null;
    let resolution: LookResolution | null = null;

    if (look !== null) {
      this.page = clampPage(look, strippedQueue, this.page, caps.handsQueue);
      resolution = resolveLook(look, {
        queue: strippedQueue,
        slots,
        page: this.page,
        handsQueue: caps.handsQueue,
        manualBoxes: this.manualBoxes
      });
    }

    // Program: stage a newly-active look in preview (never program — an
    // operator selecting a look must not force it on air by itself). Goes
    // through the engine's own `setPreview`, which already knows how to
    // degrade for a host with no preview bus.
    if (resolution !== null && resolution.lookId !== previousLookId) {
      this.setPreview({ kind: "look", lookId: resolution.lookId });
    }
    this.currentLook = resolution;

    // Overlays: re-derive every tick and keep the change flag for Task 8.
    // No `ShowEngine` input sets `question` yet, so it is always `null`
    // today (see the field's own doc comment).
    this.overlaysChanged = this.overlayDirector.update({
      look: resolution,
      question: this.question,
      questionVisible: this.questionVisible
    });

    // Tally has no state of its own to advance here — `snapshot()` already
    // derives it fresh from `this.currentLook` (now populated above) plus
    // the program source and the active-speaker-to-slot translation, which
    // only the engine can do (it is the one thing that knows both
    // `ProgramBus` and `LiveSlots`).

    const now = this.clock.now();
    const debounceElapsed =
      this.lastSaveTime === null || now - this.lastSaveTime >= SAVE_DEBOUNCE_MS;
    if (this.pendingPersist && debounceElapsed) {
      await this.store.save(this.buildPersistedState());
      this.lastSaveTime = now;
      this.pendingPersist = false;
    }

    this.tickRevision += 1;
    return this.snapshot();
  }

  /** The tick counter, starting at 0. Advanced by `tick()`. */
  revision(): number {
    return this.tickRevision;
  }

  /**
   * Assemble the published snapshot from current module state. Before the
   * first tick it is fully valid: empty roster, `look: null`, `mode: "none"`
   * tally, empty overlays, and capabilities resolved from current health.
   */
  snapshot(): ShowSnapshot {
    const slots = this.liveSlots.slots();
    const gallery = this.gallery.cells();
    const program = this.programBus.state();
    const health = this.mukanaHealth();
    const capabilities = resolveCapabilities(this.config, health);

    const panelists = buildPanelistDb(
      this.zoomIngest.snapshot(),
      this.mukanaRegistry.current(),
      this.overrideDb.entries()
    );

    const activeSpeakerSlot =
      program.activeSpeakerId === null ? null : this.liveSlots.slotOf(program.activeSpeakerId);

    const tally = deriveTally({
      source: program.program,
      slots,
      gallery,
      look: this.currentLook,
      activeSpeakerSlot
    });

    return buildSnapshot({
      revision: this.tickRevision,
      panelists,
      slots,
      gallery,
      queue: this.queue,
      program,
      look: this.currentLook,
      page: this.page,
      manualBoxes: this.manualBoxes,
      tally,
      overlays: this.overlayDirector.state(),
      capabilities,
      health,
      unseated: this.unseatedPanelists,
      pagingRefused: this.pagingRefused
    });
  }

  /** Assemble the document `tick()` persists: everything `StateStore.load`/`restore()` round-trips. */
  private buildPersistedState(): PersistedShowState {
    return {
      version: STATE_VERSION,
      slots: this.liveSlots.toJSON(),
      overrides: this.overrideDb.entries(),
      gallery: this.gallery.toJSON(),
      manualBoxes: this.manualBoxes,
      lookId: this.selectedLookId
    };
  }

  /** The current Mukana health snapshot, or the all-failing stand-in when there is no client at all. */
  private mukanaHealth(): Record<MukanaEndpoint, MukanaHealth> {
    return this.mukanaClient?.health ?? NO_REGISTRY_HEALTH;
  }

  /** Look up a `LookDefinition` by id, or `null` for a `null` id. Never throws — `setLook` is the validating gate. */
  private lookById(lookId: string | null): LookDefinition | null {
    if (lookId === null) return null;
    return this.config.looks.find((candidate) => candidate.id === lookId) ?? null;
  }
}
