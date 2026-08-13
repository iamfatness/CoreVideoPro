/**
 * The show engine — the class that owns every module instance and is the
 * first real composition in this package. Twenty-one modules were shipped
 * individually tested but never wired together; this is that wiring.
 *
 * This file covers construction, restore-from-disk, the snapshot accessor,
 * roster intake, the seating tick (Task 5), the active-speaker dispatch gate
 * (Task 6), the derived layers (Task 7): look resolution against the live
 * roster and hands queue, paging, manual box assignment, program/preview
 * transport (with degradation for a host with no preview bus), and the
 * overlay director; and host command emission (Task 8): diffing each tick's
 * derived state into `HostAdapter` calls for slots/looks/gallery/nameplates/
 * questions. The diffing itself lives in `hostCommands.ts` — this file only
 * sequences WHEN each `emit*` call runs and WHAT this tick's already-
 * resolved values are; it holds none of the "did this actually change"
 * logic. The polling loop is Task 9+.
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
import { MukanaRegistry, type DormantOutcome, type MukanaOutcome, type QuestionOutcome } from "./mukanaParse.js";
import { MukanaPoller } from "./mukanaPoller.js";
import { LiveSlots } from "./liveSlots.js";
import { GalleryDirector } from "./galleryDirector.js";
import { OverrideDb, type OverrideRecord } from "./overrideDb.js";
import { ZoomIngest, type ZoomEvent } from "./zoomIngest.js";
import { ProgramBus } from "./programBus.js";
import { OverlayDirector } from "./overlayDirector.js";
import { HostCommandEmitter } from "./hostCommands.js";
import { buildPanelistDb } from "./panelistDb.js";
import { deriveTally } from "./tallyPublisher.js";
import { resolveCapabilities } from "./capabilities.js";
import { buildSnapshot, type ShowSnapshot } from "./showSnapshot.js";
import { StateStore, STATE_VERSION, type PersistedShowState } from "./persistence.js";
import { clampPage, findChairSlots, resolveLook, type LookResolution } from "./lookDirector.js";
import { LookController } from "./lookController.js";
import { stripChairs, type HandsOutcome } from "./handsQueue.js";
import {
  EXCLUSIVE_ROLES,
  type Headline,
  type MukanaQuestion,
  type Panelist,
  type ProgramSource,
  type QueueState,
  type Role,
  type ShowCapabilities,
  type Slot
} from "./contracts.js";
import { personKeyForPin, type PersonKey } from "./personKey.js";

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
  private readonly mukanaPoller: MukanaPoller | undefined;
  private readonly assigner: PositionAssigner;
  private readonly galleryCellCount: number;

  private readonly mukanaRegistry: MukanaRegistry;
  private readonly overrideDb: OverrideDb;
  private readonly zoomIngest: ZoomIngest;
  private readonly programBus: ProgramBus;
  private readonly overlayDirector: OverlayDirector;
  private readonly hostCommands: HostCommandEmitter;
  private readonly lookController: LookController;

  private liveSlots: LiveSlots;
  private gallery: GalleryDirector;

  private tickRevision = 0;
  private currentLook: LookResolution | null = null;
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
   * The operator-set headline, or `null` before one is ever typed. Unlike a
   * nameplate, nothing derives this from the roster or the resolved look —
   * see `Headline`'s own doc comment. Not part of `PersistedShowState`
   * (mirrors `question`/`questionVisible`, which aren't either): it is
   * live-show scratch state, not something a restart should resurrect. Set
   * by `setHeadline`; fed to `OverlayDirector.update` every tick alongside
   * `headlineVisible`.
   */
  private headline: Headline | null = null;

  /**
   * Whether the operator currently wants the headline on screen. Set by
   * `setHeadlineVisible`, independently of `headline` itself —
   * `OverlayDirector` retains the text across a visibility toggle (see its
   * own doc comment), so hiding then re-showing restores the same content
   * without the operator retyping it.
   */
  private headlineVisible = false;

  /**
   * What the last `restore()` had to discard from the persisted document,
   * republished on every snapshot until another `restore()` replaces it
   * (final review, I3/I4). Empty for a clean restore and for an engine that
   * never restored. Replaced wholesale rather than appended to, so a second
   * `restore()` cannot accumulate warnings about a document it no longer
   * loaded. See `ShowSnapshot.restoreWarnings`.
   */
  private restoreWarnings: string[] = [];

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
   *
   * "When it writes" means the instant the document is BUILT, before the
   * `await` on `StateStore.save` (final review, I2): a change that lands
   * while that write is in flight is not in the document being written, so
   * it must leave the flag set for the next tick. Clearing it after the
   * await dropped exactly those changes. A failed write re-sets it.
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
    this.mukanaPoller =
      deps.mukana === undefined
        ? undefined
        : new MukanaPoller({
            client: deps.mukana,
            clock: deps.clock,
            integrations: deps.config.integrations
          });
    this.assigner = deps.assigner ?? new FiloAssigner({ capacity: deps.config.capacity });

    this.galleryCellCount = Math.min(deps.config.galleryCells, deps.host.capabilities().maxGalleryCells);

    this.mukanaRegistry = new MukanaRegistry();
    this.overrideDb = new OverrideDb();
    this.zoomIngest = new ZoomIngest();
    this.programBus = new ProgramBus({ skipRoles: deps.config.skipRoles });
    this.overlayDirector = new OverlayDirector();
    this.hostCommands = new HostCommandEmitter(deps.host);
    this.lookController = new LookController({ looks: deps.config.looks });

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
    this.lookController.select(lookId);
    this.pendingPersist = true;
    this.otherInputsChanged = true;
  }

  /**
   * Test-only escape hatch: writes the pending page directly, with **no**
   * clamping. Production code should never call this — operator paging
   * goes through `nextGuest`/`prevGuest`, and `tick()`'s own `clampPage`
   * step is what keeps the page sane between ticks, deriving the valid
   * range from the current capability state and queue every time. This
   * exists so a property test can plant a page the way a *stale* one would
   * actually arrive at the top of a tick — e.g. left over from a
   * fill-strategy flip or a look change — and prove `tick()` survives it
   * rather than throwing. Forwards to `LookController.setPage`.
   */
  setPage(page: number): void {
    this.lookController.setPage(page);
  }

  /**
   * Move the paging window forward/back by one. Only takes effect when the
   * active look's boxes are actually filling from the hands queue this
   * tick (`effectiveBoxFill(...) === "queue"`) — under manual fill (a
   * manual look, or a queue look whose hands feed just died) there is no
   * queue window to move through — AND only when the move stays inside the
   * current page range: `lookDirector.ts`'s own docs are explicit that
   * clamping a direct operator move "would silently swallow a 'next' that
   * ran off the end, which is exactly the silence an operator control must
   * not produce" (Fix round 1, Finding 3 — the pre-fix version let the page
   * walk past the end here and relied on `tick()`'s `clampPage` to quietly
   * pull it back, which is precisely that silence: the operator got no
   * signal their "next" didn't do anything new). Either refusal reason —
   * wrong fill strategy, or off the end — is recorded on
   * `LookController.refusal()` instead of throwing or silently doing
   * nothing (spec §4). The page itself is left untouched by a refused move.
   *
   * `LookController.adjustPage` only tracks selection/paging state; it does
   * not resolve capabilities or strip the queue itself, so both are
   * resolved fresh here — the SAME shape `tick()`'s own `clampPage`/
   * `resolveLook` pair would use, but resolved independently since a direct
   * operator move can land between ticks.
   */
  nextGuest(): void {
    this.adjustPage(1);
  }

  /** The mirror of `nextGuest`; see its docs. */
  prevGuest(): void {
    this.adjustPage(-1);
  }

  private adjustPage(delta: number): void {
    const caps = resolveCapabilities(this.config, this.mukanaHealth());
    const strippedQueue = this.stripQueueAgainstSeatedChairs(this.liveSlots.slots());
    this.lookController.adjustPage(delta, strippedQueue, caps.handsQueue);
  }

  /**
   * Write or overwrite one manual box assignment. Meaningful only under
   * manual box fill (`resolveLook` simply ignores manual assignments for a
   * look currently filling from the queue), but recorded unconditionally —
   * the operator may be setting it up in advance of a fill-strategy switch.
   * Throws for a box number outside the ACTIVE look's `1..boxes` range —
   * caller error, not a state-changed-under-me refusal, so it throws rather
   * than getting a typed refusal response. When no look is selected yet
   * there is no range to validate against, so only a non-positive-integer
   * box number is rejected. Forwards to `LookController.assignBox`.
   */
  assignBox(box: number, slot: number): void {
    this.lookController.assignBox(box, slot);
    this.pendingPersist = true;
  }

  /** Remove one manual box assignment, leaving the rest untouched. Forwards to `LookController.clearBox`. */
  clearBox(box: number): void {
    this.lookController.clearBox(box);
    this.pendingPersist = true;
  }

  /** Toggle whether the audience question overlay should render. Forwarded to `OverlayDirector.update` every tick. */
  setQuestionVisible(on: boolean): void {
    this.questionVisible = on;
  }

  /**
   * Set (or clear, with `null`) the operator headline's text. Visibility is
   * a separate control (`setHeadlineVisible`) — setting the text alone does
   * not put it on screen, matching `Headline`'s "operator-driven, not
   * derived" design (contracts.ts).
   */
  setHeadline(headline: Headline | null): void {
    this.headline = headline;
  }

  /** Toggle whether the headline overlay should render. Forwarded to `OverlayDirector.update` every tick; no gating of its own. */
  setHeadlineVisible(on: boolean): void {
    this.headlineVisible = on;
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
   * Seat `participantId` into `slot`, or the first empty slot when `slot` is
   * omitted. This is one of the operator's explicit seat controls the
   * file-level doc comment on this class refers to — the reason a Zoom
   * departure never vacates a seat (owner ruling, 2026-08-06): clearing or
   * changing a seat is always this kind of explicit call, never an
   * automatic consequence of a connection blip. Looks `participantId` up in
   * the current published panelist join (`requirePanelist` — the same Zoom
   * x Mukana x overrides join `tick()`'s seat step and `buildSnapshotFrom`
   * build fresh every time), so the seated record carries this show's
   * resolved name/role, never a bare id; throws for a participant this show
   * has not published.
   *
   * An explicit occupied `slot` is refused — thrown, never silently
   * overwritten. `replacePanelist` is the verb for that; an operator who
   * meant to add and hit an occupied slot must find out, not have their
   * intent silently reinterpreted. An out-of-range `slot` is left to
   * `LiveSlots.replace`'s own `assertSlot` rather than duplicated here.
   */
  addPanelist(participantId: string, slot?: number): number | null {
    const panelist = this.requirePanelist(participantId);

    if (slot === undefined) {
      const seated = this.liveSlots.add(panelist);
      if (seated !== null) this.markSeatingDirty();
      return seated;
    }

    const occupant = this.liveSlots.slots().find((candidate) => candidate.slot === slot)?.panelist;
    if (occupant !== undefined && occupant !== null) {
      throw new Error(
        `addPanelist: slot ${slot} is already occupied by ${occupant.participantId} — use replacePanelist to overwrite it`
      );
    }
    this.liveSlots.replace(slot, panelist);
    this.markSeatingDirty();
    return slot;
  }

  /**
   * Clear a seat. THE explicit operator action the file-level doc comment
   * refers to: a Zoom departure never reaches this path — it only flips a
   * seated panelist's `online`/`videoOn` (`LiveSlots.refresh`, driven from
   * `tick()`'s seat step) — so only an operator calling this ever vacates a
   * slot.
   */
  removePanelist(slot: number): void {
    this.liveSlots.removeSlot(slot);
    this.markSeatingDirty();
  }

  /**
   * Overwrite whoever holds `slot` with `participantId`, regardless of
   * whether it was occupied — the explicit verb `addPanelist` refuses to
   * be. Resolves `participantId` the same way `addPanelist` does; throws
   * for an unknown participant or an out-of-range slot.
   */
  replacePanelist(slot: number, participantId: string): void {
    const panelist = this.requirePanelist(participantId);
    this.liveSlots.replace(slot, panelist);
    this.markSeatingDirty();
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
   * The PIN-keyed twin of `setOverride` (spec §4.2's `ohg.panelist.role.set`,
   * a `Role` narrower than `setOverride`'s free-form `OverrideRecord`).
   * Takes the **PIN**, never a `PersonKey` — resolved through
   * `personKeyForPin`, the one sanctioned way to reach a PIN-tier key from a
   * raw PIN (`personKey.ts`). Never build the key any other way: a
   * `PersonKey` is opaque and must not be taken apart to recover or rebuild
   * one, the exact mistake that module's doc comment calls out.
   *
   * `displayName`/`location` are pulled from the Mukana registry entry for
   * this PIN when one exists, else from whatever override this person
   * already carries (so re-assigning a role never blanks a name an earlier
   * override recorded), else left empty — the same fallback
   * `OverrideDb.assignExclusiveRole` already uses for a demoted holder, and
   * just as harmless here: `buildPanelistDb`'s `pick` treats an empty
   * override field as absent and falls through to the Mukana/Zoom-parsed
   * name, so this never blanks what is actually shown on screen.
   *
   * For an exclusive role (`"host"`/`"reader"`), `OverrideDb.set` alone
   * would happily leave two hosts — `assignExclusiveRole` is the only thing
   * that demotes a prior holder, exactly mirroring `setOverride` above.
   */
  setRole(pin: string, role: Role): void {
    const personKey = personKeyForPin(pin);
    const registryRecord = this.mukanaRegistry.current()[pin];
    const priorRecord = this.overrideDb.entries()[personKey];
    this.overrideDb.set({
      personKey,
      displayName: registryRecord?.displayName ?? priorRecord?.displayName ?? "",
      location: registryRecord?.location ?? priorRecord?.location ?? "",
      role
    });
    if (isExclusiveOverrideRole(role)) {
      this.overrideDb.assignExclusiveRole(personKey, role, this.mukanaRegistry.current());
    }
    this.markSeatingDirty();
  }

  /**
   * Force every configured Mukana endpoint to poll on the very next tick,
   * without waiting out its interval or backoff — the operator's "sync now"
   * control (spec §4.2's `ohg.panelist.syncAll`/`ohg.mukana.sync`). A show
   * with no Mukana client (`this.mukanaPoller` undefined) has nothing to
   * force; this degrades to a no-op rather than an error, matching how
   * every other Mukana-facing method on this class treats a registry-less
   * show. Forwards to `MukanaPoller.forceDue` — see its own doc comment for
   * the mechanism (it starts nothing itself; it only clears the due-check's
   * anchor for the NEXT `poll()`, which `tick()` always calls).
   */
  syncAll(): void {
    this.mukanaPoller?.forceDue();
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
   *
   * Three outcomes, not two (final review, I4). A `LiveSlotsRestoreError` or
   * `GalleryError` from a structurally-INCOHERENT document — a seat whose
   * `slot` disagrees with its index, an `assignments` array whose length
   * disagrees with its own `cells` — is a real corruption and still
   * propagates rather than silently downgrading to an empty roster. But a
   * count that disagrees with THIS configuration is not corruption at all:
   * `galleryCellCount` is `min(config.galleryCells, host.maxGalleryCells)`,
   * so the same good state file restored on a host that renders fewer
   * gallery cells — the spec's own degradation story — or after an operator
   * edits `capacity`/`galleryCells`, threw `GalleryError` at startup and
   * reported a legitimate config change as a broken file. Those two cases
   * are now detected BEFORE `fromJSON` (by comparing the counts directly,
   * rather than by catching an error and reading its message), the stale
   * geometry is discarded, and the discard is reported on the published
   * snapshot's `restoreWarnings` — recoverable, never silent.
   *
   * The same treatment covers a persisted `lookId` this configuration no
   * longer defines (I3): adopting it left NO look resolving for the rest of
   * the show with nothing warning, and re-persisted the phantom id so every
   * later restart repeated it. `setLook` throws for the same input, so this
   * path may not be quieter than that — it drops the id, drops the manual
   * boxes that belonged to it (via the comparison below), says so, and
   * marks the state dirty so the corrected document reaches disk.
   */
  async restore(): Promise<boolean> {
    const persisted = await this.store.load();
    if (persisted === null) return false;

    const warnings: string[] = [];

    let restoredSlots: LiveSlots;
    if (persisted.slots.capacity === this.config.capacity) {
      restoredSlots = LiveSlots.fromJSON(persisted.slots, {
        capacity: this.config.capacity,
        utilityPinBase: this.config.utilityPinBase
      });
    } else {
      warnings.push(
        `persisted roster capacity ${persisted.slots.capacity} does not match this show's configured capacity ` +
          `${this.config.capacity}; the persisted seating was discarded and the roster starts empty`
      );
      restoredSlots = new LiveSlots({
        capacity: this.config.capacity,
        utilityPinBase: this.config.utilityPinBase
      });
    }

    let restoredGallery: GalleryDirector;
    if (persisted.gallery.cells === this.galleryCellCount) {
      restoredGallery = GalleryDirector.fromJSON(persisted.gallery, {
        cells: this.galleryCellCount
      });
    } else {
      warnings.push(
        `persisted gallery of ${persisted.gallery.cells} cell(s) does not match this show's ${this.galleryCellCount} ` +
          `(configured gallery cells, clamped to what this host can render); the persisted arrangement was ` +
          `discarded and the gallery starts empty`
      );
      restoredGallery = new GalleryDirector({ cells: this.galleryCellCount });
    }

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

    // A look that is no longer in this configuration cannot be adopted: it
    // would resolve to nothing, forever, and be written straight back to
    // disk on the next save (final review, I3). Dropped and reported.
    // `LookController.adoptRestored` also carries Fix round 1's "also fix":
    // before that method existed, `persisted.lookId` was write-only —
    // nothing ever read it back — so a genuine cold restart (`restore()`
    // with no `setLook()` first) resolved no look at all until an operator
    // re-selected one. But a `setLook()` issued BEFORE `restore()` is a
    // deliberate choice made after the process came back up, and must win
    // over whatever was on disk — the same ordering contract
    // `restoreManualBoxes` below already depends on (see the restore tests:
    // `setLook("teatime")` then `restore()` from a file whose `lookId`
    // differs must still leave "teatime" selected).
    const lookWarning = this.lookController.adoptRestored(persisted.lookId);
    if (lookWarning !== null) {
      warnings.push(lookWarning);
    }

    // A restored assignment set belongs to whatever look was selected when
    // it was saved. Applying it under a different look would put whoever
    // the operator put in box 1 of one arrangement into box 1 of another,
    // which is not the same seat — discard rather than inherit.
    // `restoreManualBoxes` compares `persisted.lookId` against whatever
    // `adoptRestored` just selected, so a cold restart (which just adopted
    // `persisted.lookId` verbatim) always matches and restores its own
    // manual boxes, while an explicit pre-restore `setLook()` to a
    // different look still discards them.
    this.lookController.restoreManualBoxes(persisted.manualBoxes, persisted.lookId);

    this.restoreWarnings = warnings;
    if (warnings.length > 0) {
      // Self-heal the document. Without this the discarded look id / stale
      // geometry stays on disk until some unrelated change happens to
      // trigger a save, so a restart could keep re-reading (and
      // re-reporting) the same stale values — the "it never self-heals"
      // half of I3.
      this.pendingPersist = true;
    }

    return true;
  }

  /**
   * The heartbeat. Runs in a fixed order: poll Mukana (Task 9 — start any
   * due fetch and apply whatever settled since the last tick, never
   * blocking on one in flight), commit the buffered roster, seat it
   * (holding seats still for an unchanged participant set, reseating only
   * when it actually changed), dispatch a pending active speaker (Task 6),
   * recompute the derived layers — look resolution, paging, program
   * staging, overlays (Task 7) — persist if the debounce window allows it,
   * emit host commands (Task 8), then advance the revision and publish.
   *
   * Step 1 alone (`ZoomIngest.commit()`) can say nothing changed, but the
   * tick still runs to completion — a look or capability change can alter
   * output against a static roster, and the revision always advances so a
   * consumer polling `revision()` can tell every tick apart. Host emission
   * runs every tick too, unconditionally — `hostCommands` is what decides
   * whether any given call is actually worth sending this time.
   *
   * **This promise can reject, and the host's tick loop must handle it.**
   * No external integration can make it reject — that is the whole point of
   * `MukanaPoller.poll` being await-free — but the injected `StateFs` can: a
   * full or read-only disk fails the debounced save and that failure
   * propagates deliberately, because silently continuing would mean a show
   * whose state has stopped persisting with nothing saying so. The engine
   * stays usable (the dirty flag is restored, so the next tick past the
   * debounce retries), but an unhandled rejection here kills the process
   * under Node's defaults. A host must wrap its tick call and surface the
   * failure to the operator.
   */
  async tick(): Promise<ShowSnapshot> {
    // Mukana polling (Task 9, carved into `MukanaPoller` in Task 1) runs
    // FIRST, before anything else this tick reads: `poll()` starts any due,
    // non-busy fetch, then `drain()` returns whatever settled since the last
    // tick. A settled panelists outcome applies through the existing
    // `onMukanaPanelists`, which flips `otherInputsChanged`/`pendingPersist`
    // — those must be set before `otherChanged` is captured immediately
    // below, or a registry update that just landed would not affect the
    // seat step until a tick later than it actually could. Hands/question
    // outcomes replace `this.queue`/`this.question` directly, which the
    // derived-layers step further down reads — so this must also run
    // before that. See `MukanaPoller.poll`'s own doc comment for the
    // scheduling and non-blocking rules themselves.
    if (this.mukanaPoller !== undefined) {
      this.mukanaPoller.poll();
      const outcomes = this.mukanaPoller.drain();
      if (outcomes.panelists !== null) this.onMukanaPanelists(outcomes.panelists);
      if (outcomes.hands !== null) this.applyHandsOutcome(outcomes.hands);
      if (outcomes.question !== null) this.applyQuestionOutcome(outcomes.question);
    }

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
        // DEFERRED, deliberately (final review, Minor): the assigner's
        // returned `PlacementChange[]` is discarded and `positions()` has no
        // caller anywhere in the package, so nothing downstream of this gate
        // observes the speaker pool. The Plan 3 obligation is discharged AT
        // THIS BOUNDARY — a skipped role never reaches the assigner, which
        // is what the property in `speakerGateDispatch.test.ts` proves — but
        // the visible half of the original defect ("an ASL interpreter sorts
        // to the front of the gallery") cannot manifest OR be prevented yet,
        // because no gallery or slot state is driven from recency. Wiring
        // recency into the gallery is Plan 6's call; until it exists, the
        // assigner is a correctly-gated pool nobody reads.
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
    //
    // Fix round 1, Finding 2: `health`/`caps` are captured here and carried
    // all the way to THIS tick's own published snapshot at the bottom
    // (`buildSnapshotFrom(caps, health)`), rather than the public
    // `snapshot()` re-resolving from the live `mukanaClient.health` getter
    // after `await this.store.save(...)` below. That getter can change
    // mid-tick from an async fetch landing during the await — a poll
    // resolving `hands` from failing to ok right then would otherwise let
    // `capabilities.handsQueue` in the published snapshot disagree with the
    // `look.boxFill`/`pageCount` computed earlier in this SAME tick from
    // the pre-await value, publishing a snapshot that contradicts itself.
    const health = this.mukanaHealth();
    const caps = resolveCapabilities(this.config, health);

    const slots = this.liveSlots.slots();
    const strippedQueue = this.stripQueueAgainstSeatedChairs(slots);

    const look = this.lookController.selected();
    const previousLookId = this.currentLook?.lookId ?? null;
    let resolution: LookResolution | null = null;

    if (look !== null) {
      this.lookController.setPage(clampPage(look, strippedQueue, this.lookController.page(), caps.handsQueue));
      resolution = resolveLook(look, {
        queue: strippedQueue,
        slots,
        page: this.lookController.page(),
        handsQueue: caps.handsQueue,
        manualBoxes: this.lookController.manualBoxes()
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

    // Fix round 1, Finding 4: a "no-look"/"fill" paging refusal is only
    // meaningful while its cause persists — the moment a look IS resolved
    // (clearing "no-look"), or its boxes are ACTUALLY filling from the
    // queue again (hands feed recovered, clearing "fill"), a refusal
    // recorded while that wasn't true is stale and must not keep
    // publishing. `nextGuest`/`prevGuest` are the only other writers of a
    // refusal, and neither runs every tick, so nothing else would clear it
    // otherwise. A `"range"` refusal is deliberately NOT cleared here — see
    // `LookController.clearStaleRefusal`'s doc comment for why (it would
    // wipe out on the very next tick, since queue fill is the only strategy
    // an out-of-range move can even happen under). Only called when a look
    // actually resolved this tick — `clearStaleRefusal` treats being called
    // at all as proof a look is selected, matching the `resolution !== null`
    // guard both refusal kinds required here before the carve-out.
    if (resolution !== null) {
      this.lookController.clearStaleRefusal(resolution.boxFill);
    }

    // Overlays: re-derive every tick and keep the change flag for Task 8.
    // No `ShowEngine` input sets `question` yet, so it is always `null`
    // today (see the field's own doc comment). `headline`/`headlineVisible`
    // are the operator's own state, written by `setHeadline`/
    // `setHeadlineVisible`.
    this.overlaysChanged = this.overlayDirector.update({
      look: resolution,
      question: this.question,
      questionVisible: this.questionVisible,
      headline: this.headline,
      headlineVisible: this.headlineVisible
    });

    // Tally has no state of its own to advance here — the final snapshot
    // build below already derives it fresh from `this.currentLook` (now
    // populated above) plus the program source and the
    // active-speaker-to-slot translation, which only the engine can do (it
    // is the one thing that knows both `ProgramBus` and `LiveSlots`).

    const now = this.clock.now();
    const debounceElapsed =
      this.lastSaveTime === null || now - this.lastSaveTime >= SAVE_DEBOUNCE_MS;
    if (this.pendingPersist && debounceElapsed) {
      // Clear `pendingPersist` BEFORE the await, not after (final review,
      // I2). The document handed to `save` is built synchronously here, so
      // it captures state as of this instant; any operator input that lands
      // DURING the write is not in it, and clearing the flag afterwards
      // wiped exactly that input's dirty mark. Measured with a deferred
      // `writeFile`: an `assignBox` issued inside the write window was
      // acknowledged in memory and then never written — three later ticks
      // with the debounce fully elapsed produced no second write, and a
      // restart lost it. `assignBox`/`clearBox` are the exposed case
      // because they set only this flag (`setLook`/`setOverride`/
      // `clearOverride`/`onMukanaPanelists` also set `otherInputsChanged`,
      // which the seat step re-dirties), and manual boxes are precisely the
      // fallback the hands-feed degradation path depends on.
      this.pendingPersist = false;
      this.lastSaveTime = now;
      try {
        await this.store.save(this.buildPersistedState());
      } catch (error) {
        // The write did not land, so this state is still unsaved: restore
        // the dirty mark before propagating (see `tick()`'s doc comment —
        // a `StateFs` failure is the host's to handle, not something this
        // engine may swallow). `lastSaveTime` is deliberately NOT rolled
        // back: a failing disk should be retried on the normal debounce
        // cadence, not on every tick.
        this.pendingPersist = true;
        throw error;
      }
    }

    // Host command emission (Task 8). `hostCommands` owns diffing this
    // tick's derived state against what it last actually sent, so this is
    // NOT a full re-send every tick — see `hostCommands.ts` for why that
    // distinction matters on a live show. `slots`/`this.currentLook` are
    // this SAME tick's already-resolved values (no re-derivation), and
    // nameplates/question ride `overlaysChanged`, the change flag
    // `overlayDirector.update` already computed above — never re-diffed
    // here.
    this.hostCommands.emitSlots(slots);
    this.hostCommands.emitLook(this.currentLook);
    this.hostCommands.emitGallery(this.gallery.cells());
    this.hostCommands.emitOverlays(this.overlayDirector.state(), this.overlaysChanged);

    this.tickRevision += 1;
    return this.buildSnapshotFrom(caps, health);
  }

  /** The tick counter, starting at 0. Advanced by `tick()`. */
  revision(): number {
    return this.tickRevision;
  }

  /**
   * Assemble the published snapshot from current module state, resolving
   * capabilities fresh from the live health getter. This is the right
   * behavior for an out-of-tick caller (e.g. right after construction,
   * before the first `tick()`), but `tick()` itself must NOT call this —
   * it calls the private `buildSnapshotFrom` with the SAME `caps`/`health`
   * it already resolved once at the top of that tick (Fix round 1, Finding
   * 2). Before the first tick this is fully valid: empty roster, `look:
   * null`, `mode: "none"` tally, empty overlays, and capabilities resolved
   * from current health.
   */
  snapshot(): ShowSnapshot {
    const health = this.mukanaHealth();
    const capabilities = resolveCapabilities(this.config, health);
    return this.buildSnapshotFrom(capabilities, health);
  }

  /**
   * The actual snapshot assembly, parameterized on an already-resolved
   * `capabilities`/`health` pair so a caller (`tick()`, or the public
   * `snapshot()` above) controls whether that pair is freshly resolved or
   * carried over from earlier work. Never resolves capabilities itself.
   */
  private buildSnapshotFrom(
    capabilities: ShowCapabilities,
    health: Record<MukanaEndpoint, MukanaHealth>
  ): ShowSnapshot {
    const slots = this.liveSlots.slots();
    const gallery = this.gallery.cells();
    const program = this.programBus.state();

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
      page: this.lookController.page(),
      manualBoxes: this.lookController.manualBoxes(),
      tally,
      overlays: this.overlayDirector.state(),
      capabilities,
      health,
      unseated: this.unseatedPanelists,
      pagingRefused: this.lookController.refusal()?.message ?? null,
      restoreWarnings: this.restoreWarnings
    });
  }

  /** Assemble the document `tick()` persists: everything `StateStore.load`/`restore()` round-trips. */
  private buildPersistedState(): PersistedShowState {
    return {
      version: STATE_VERSION,
      slots: this.liveSlots.toJSON(),
      overrides: this.overrideDb.entries(),
      gallery: this.gallery.toJSON(),
      manualBoxes: this.lookController.manualBoxes(),
      lookId: this.lookController.selected()?.id ?? null
    };
  }

  /**
   * Apply one hands-queue fetch outcome. Only `"data"` replaces
   * `this.queue` — `dormant`/`invalid` leave the last-good queue in place;
   * `MukanaClient` already recorded the health transition for
   * `capabilities.handsQueue`, so there is nothing more to do here than
   * decline to overwrite good data with nothing (spec: discarding good
   * data over one failed poll is the exact incident this design prevents).
   * Unlike `onMukanaPanelists`, this never sets `pendingPersist`/
   * `otherInputsChanged`: the raw hands queue is not part of persisted
   * state (`buildPersistedState` does not carry it) and does not affect
   * roster seating, only paging/box-fill, which `tick()`'s derived-layers
   * step already recomputes fresh every tick regardless.
   */
  private applyHandsOutcome(outcome: HandsOutcome | DormantOutcome): void {
    if (outcome.kind !== "data") return;
    this.queue = outcome.queue;
  }

  /** The question-feed twin of `applyHandsOutcome` — see its doc comment for the retention rule. */
  private applyQuestionOutcome(outcome: QuestionOutcome): void {
    if (outcome.kind !== "data") return;
    this.question = outcome.question;
  }

  /**
   * Look `participantId` up in the current published panelist join — the
   * same Zoom x Mukana registry x overrides join `tick()`'s seat step and
   * `buildSnapshotFrom` build fresh every time, never a stale one carried
   * from an earlier call. Shared by `addPanelist`/`replacePanelist`. Throws
   * for a participant this show has never published (a stale or mistyped id
   * from an operator control): seating a synthetic stand-in would be worse
   * than refusing outright.
   */
  private requirePanelist(participantId: string): Panelist {
    const panelist = buildPanelistDb(
      this.zoomIngest.snapshot(),
      this.mukanaRegistry.current(),
      this.overrideDb.entries()
    ).get(participantId);
    if (panelist === undefined) {
      throw new Error(`unknown participant id ${JSON.stringify(participantId)}`);
    }
    return panelist;
  }

  /**
   * Mark both the seat-step-rerun and persisted-state dirty flags together
   * — shared by every explicit seat control above (`addPanelist`/
   * `removePanelist`/`replacePanelist`/`setRole`). Unlike `assignBox`/
   * `clearBox`, which set only `pendingPersist` (see that field's own doc
   * comment), a direct `LiveSlots` mutation or an override change can leave
   * other seats' displayed data stale until the seat step's `refresh` next
   * runs, so both flags always go together here.
   */
  private markSeatingDirty(): void {
    this.pendingPersist = true;
    this.otherInputsChanged = true;
  }

  /**
   * The current Mukana health snapshot, or the all-failing stand-in when
   * there is no client at all — with one engine-side correction the client
   * cannot make for itself: an endpoint whose in-flight poll has been
   * outstanding beyond the hung threshold reads `failing` regardless of what
   * its last settled response said (final review, I1 — see `MukanaPoller`'s
   * own doc comment for why, and `FetchLike`'s for the timeout obligation
   * this backstops).
   *
   * That correction is applied by CALLING `detectHungEndpoints()` for its
   * side effect and then reading `client.health` fresh, rather than reading
   * `client.health` first and overriding entries from its return value (fix
   * round 1, Minor 2): `detectHungEndpoints()` already writes any hung
   * endpoint's health directly, via `MukanaClient.markHung` — building a
   * SECOND copy of that same override here was redundant the moment it
   * gained that side effect, and worse, ORDER-DEPENDENT: reading
   * `client.health` before calling `detectHungEndpoints()` only happened to
   * work because the override loop below unconditionally stomped whatever
   * stale snapshot it read. Calling it first means there is exactly ONE
   * place this state is ever written, and this method cannot silently stop
   * doing anything by a reordering.
   *
   * Safe to return directly: `MukanaClient.health` builds a fresh object
   * with fresh per-endpoint records on every read, so nothing here can be
   * mutated out from under a caller. The registry-less `NO_REGISTRY_HEALTH`
   * branch returns the shared module constant by reference and is
   * deliberately left untouched — there is no client, so nothing can be in
   * flight.
   */
  private mukanaHealth(): Record<MukanaEndpoint, MukanaHealth> {
    const client = this.mukanaClient;
    if (client === undefined || this.mukanaPoller === undefined) return NO_REGISTRY_HEALTH;

    this.mukanaPoller.detectHungEndpoints();
    return client.health;
  }

  /**
   * `this.queue` with the seated host's and reader's PINs removed, so the
   * hands queue never double-books whoever already has a dedicated chair.
   * Shared by `tick()` (feeding `clampPage`/`resolveLook`) and `adjustPage`
   * (Fix round 1, Finding 3 — bounding a direct "next"/"prev" move needs
   * the SAME effective queue those two would resolve against, not the raw
   * one), so the two can never derive a different page count for the
   * identical underlying state.
   */
  private stripQueueAgainstSeatedChairs(slots: readonly Slot[]): QueueState {
    const { hostSlot, readerSlot } = findChairSlots(slots);
    return stripChairs(this.queue, {
      hostPin: pinAtSlot(slots, hostSlot),
      readerPin: pinAtSlot(slots, readerSlot)
    });
  }
}
