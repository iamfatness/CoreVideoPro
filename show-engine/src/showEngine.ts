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
import {
  clampPage,
  effectiveBoxFill,
  findChairSlots,
  pageCountFor,
  resolveLook,
  type LookResolution,
  type ManualBoxAssignments
} from "./lookDirector.js";
import { stripChairs, type HandsOutcome } from "./handsQueue.js";
import {
  EXCLUSIVE_ROLES,
  type LookDefinition,
  type MukanaQuestion,
  type Panelist,
  type ProgramSource,
  type QueueState,
  type Role,
  type ShowCapabilities,
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

/**
 * Turn a rejection reason from a Mukana fetch promise into the same
 * `string` shape `MukanaClient.fail` uses for a caught transport error —
 * `pollMukana`'s rejection handlers use this so a rejected fetch (a
 * contract-violating `FetchLike`, or anything else outside what
 * `MukanaClient.request`'s own try/catch covers) still records an ordinary
 * `invalid` outcome instead of an unhandled promise rejection.
 */
function mukanaRejectionReason(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
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
  private readonly hostCommands: HostCommandEmitter;

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
   * `null` when the last paging attempt (if any) succeeded. Written by
   * `nextGuest`/`prevGuest`, and cleared early by `tick()` and `setLook`
   * for the two causes that can go stale on their own (see
   * `pagingRefusedKind` below) — Fix round 1, Finding 4. A refusal recorded
   * between ticks is otherwise visible on the snapshot the following tick
   * publishes.
   */
  private pagingRefused: string | null = null;

  /**
   * WHY `pagingRefused` was set, tracked separately from the human-readable
   * string so `tick()` can decide which refusals it's allowed to clear
   * without parsing its own message. `"fill"` (the active look's boxes
   * aren't currently filling from the queue) and `"no-look"` (no look was
   * selected at all) are both facts about CURRENT state that a later tick
   * can independently re-check and clear once no longer true — a dead
   * hands feed recovering, or a look finally getting selected. `"range"`
   * (the attempted move ran off the end of the current page window) is
   * NOT auto-cleared by `tick()`: it was true about a specific attempted
   * move, not a standing condition, and clearing it merely because the
   * look still fills from the queue (the ONLY fill strategy under which an
   * out-of-range move is even possible) would wipe it out on the very next
   * tick — before an operator polling on any normal cadence could ever see
   * it. Only a subsequent `nextGuest`/`prevGuest` (success or another
   * refusal) or `setLook` clears a `"range"` refusal.
   */
  private pagingRefusedKind: "no-look" | "fill" | "range" | null = null;

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

  /**
   * Wall-clock time (per the injected `Clock`) each endpoint's poll was last
   * STARTED, keyed so `nextDelayMs`'s own interval/backoff math (which the
   * engine deliberately adds no policy on top of) is checked against a fresh
   * anchor every time a fetch is kicked off — never against when it
   * SETTLED, since a poll that's still in flight must not look perpetually
   * "not due yet" once its interval has genuinely elapsed. `-Infinity`
   * makes every endpoint due on the very first tick that reaches it.
   */
  private readonly lastMukanaPollAt: Record<MukanaEndpoint, number> = {
    panelists: Number.NEGATIVE_INFINITY,
    hands: Number.NEGATIVE_INFINITY,
    question: Number.NEGATIVE_INFINITY
  };

  /**
   * Whether a fetch for this endpoint is currently in flight — gates
   * starting a NEW one in `pollMukana`. Set the moment a fetch starts,
   * cleared in that SAME fetch's `.then()`/rejection handler, never
   * anywhere else. This is what makes "one in-flight promise per endpoint"
   * a real invariant rather than a comment: without it, a slow or hung
   * endpoint gets a fresh overlapping fetch every time its interval elapses
   * (unbounded concurrent requests against an already-struggling registry,
   * and — because outcomes are applied in SETTLE order, not START order — a
   * later-settling but earlier-started fetch can overwrite a fresher one
   * with stale data, e.g. the hands queue jumping backwards to an older
   * guest). `nextDelayMs`'s backoff-after-failure ceiling bounds the RATE a
   * healthy-but-slow endpoint gets hit; this bounds the COUNT in flight at
   * once to exactly one, which backoff alone does not.
   */
  private readonly mukanaPollBusy: Record<MukanaEndpoint, boolean> = {
    panelists: false,
    hands: false,
    question: false
  };

  /**
   * The outcome of a hands/question fetch that has settled since it was
   * started, waiting for the next `tick()` to notice and apply it — `null`
   * once applied, or while no fetch for that endpoint has settled yet.
   * `tick()` never `await`s the fetch itself (spec §2: no external
   * integration failure may block a tick); a background `.then()` attached
   * the moment the fetch is started writes here as its ONLY job, so writing
   * it can never race `tick()`'s own synchronous read-and-clear at the top
   * of the next `tick()` call — both run on the same single JS thread, and
   * a `.then()` callback body itself never runs concurrently with `tick()`'s
   * own execution. Panelists outcomes reuse the existing `onMukanaPanelists`
   * apply path instead of a slot like this one, because that path also has
   * to flip `otherInputsChanged`/`pendingPersist`, which must happen before
   * `tick()` captures `otherInputsChanged` for this tick's seat-step
   * decision — see the top of `tick()`.
   */
  private panelistsPollSettled: MukanaOutcome | null = null;
  private handsPollSettled: HandsOutcome | DormantOutcome | null = null;
  private questionPollSettled: QuestionOutcome | null = null;

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
    this.hostCommands = new HostCommandEmitter(deps.host);

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
    // A paging refusal recorded against the PREVIOUS look (or against no
    // look at all) has nothing to say about this one — Fix round 1,
    // Finding 4: it must not survive a look change and read as a stale,
    // misattributed reason once the new look's own paging (or lack of it)
    // is what's actually in effect.
    this.pagingRefused = null;
    this.pagingRefusedKind = null;
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
    if (!Number.isInteger(page)) {
      throw new Error(`ShowEngine.setPage: page ${page} is invalid: page must be an integer`);
    }
    this.page = page;
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
   * not produce" (Fix round 1, Finding 3 — the pre-fix version let
   * `this.page` walk past the end here and relied on `tick()`'s
   * `clampPage` to quietly pull it back, which is precisely that silence:
   * the operator got no signal their "next" didn't do anything new). Either
   * refusal reason — wrong fill strategy, or off the end — is recorded in
   * `pagingRefused` instead of throwing or silently doing nothing (spec
   * §4). `this.page` itself is left untouched by a refused move.
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
      this.pagingRefusedKind = "no-look";
      return;
    }

    const caps = resolveCapabilities(this.config, this.mukanaHealth());
    const fill = effectiveBoxFill(look, caps.handsQueue);
    if (fill !== "queue") {
      this.pagingRefused = `paging refused: box fill is ${fill}, not queue-driven`;
      this.pagingRefusedKind = "fill";
      return;
    }

    const slots = this.liveSlots.slots();
    const strippedQueue = this.stripQueueAgainstSeatedChairs(slots);
    const pageCount = pageCountFor(look, strippedQueue);
    const target = this.page + delta;

    if (target < 0 || target >= pageCount) {
      this.pagingRefused = `paging refused: page ${target} is out of range (this look has ${pageCount} page(s))`;
      this.pagingRefusedKind = "range";
      return;
    }

    this.pagingRefused = null;
    this.pagingRefusedKind = null;
    this.page = target;
  }

  /**
   * Write or overwrite one manual box assignment. Meaningful only under
   * manual box fill (`resolveLook` simply ignores manual assignments for a
   * look currently filling from the queue), but recorded unconditionally —
   * the operator may be setting it up in advance of a fill-strategy switch.
   * Throws for a box number outside the ACTIVE look's `1..boxes` range —
   * caller error, not a state-changed-under-me refusal, so it throws rather
   * than getting a typed `pagingRefused`-style response. When no look is
   * selected yet there is no range to validate against, so only a
   * non-positive-integer box number is rejected.
   */
  assignBox(box: number, slot: number): void {
    const look = this.lookById(this.selectedLookId);
    if (!Number.isInteger(box) || box < 1 || (look !== null && box > look.boxes)) {
      throw new Error(
        look === null
          ? `ShowEngine.assignBox: box ${box} is invalid: box must be a positive integer`
          : `ShowEngine.assignBox: box ${box} is out of range for look ${JSON.stringify(look.id)} (1..${look.boxes})`
      );
    }
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

    // Adopt the persisted look selection — but ONLY when nothing has
    // explicitly selected one yet (`selectedLookId` is still its field-init
    // `null`). Fix round 1, "also fix": before this line, `persisted.lookId`
    // was write-only — nothing ever read it back — so a genuine cold
    // restart (`restore()` with no `setLook()` first) resolved no look at
    // all until an operator re-selected one. But a `setLook()` issued
    // BEFORE `restore()` is a deliberate choice made after the process came
    // back up, and must win over whatever was on disk — the same ordering
    // contract `manualBoxes` below already depends on (see the restore
    // tests: `setLook("teatime")` then `restore()` from a file whose
    // `lookId` differs must still leave "teatime" selected).
    this.selectedLookId = this.selectedLookId ?? persisted.lookId;

    // A restored assignment set belongs to whatever look was selected when
    // it was saved. Applying it under a different look would put whoever
    // the operator put in box 1 of one arrangement into box 1 of another,
    // which is not the same seat — discard rather than inherit. Compared
    // against `this.selectedLookId` AFTER the adoption above, so a cold
    // restart (which just adopted `persisted.lookId` verbatim) always
    // matches and restores its own manual boxes, while an explicit
    // pre-restore `setLook()` to a different look still discards them.
    this.manualBoxes =
      persisted.lookId === this.selectedLookId ? { ...persisted.manualBoxes } : {};

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
   */
  async tick(): Promise<ShowSnapshot> {
    // Mukana polling (Task 9) runs FIRST, before anything else this tick
    // reads. A settled panelists outcome applies through the existing
    // `onMukanaPanelists`, which flips `otherInputsChanged`/`pendingPersist`
    // — those must be set before `otherChanged` is captured immediately
    // below, or a registry update that just landed would not affect the
    // seat step until a tick later than it actually could. Hands/question
    // outcomes replace `this.queue`/`this.question` directly, which the
    // derived-layers step further down reads — so this must also run
    // before that. See `pollMukana`'s own doc comment for the scheduling
    // and non-blocking rules themselves.
    if (this.mukanaClient !== undefined) {
      this.pollMukana(this.mukanaClient);
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

    // Fix round 1, Finding 4: a "no-look"/"fill" paging refusal is only
    // meaningful while its cause persists — the moment a look IS resolved
    // (clearing "no-look"), or its boxes are ACTUALLY filling from the
    // queue again (hands feed recovered, clearing "fill"), a refusal
    // recorded while that wasn't true is stale and must not keep
    // publishing. `nextGuest`/`prevGuest` are the only other writers of
    // `pagingRefused`, and neither runs every tick, so nothing else would
    // clear it otherwise. A `"range"` refusal is deliberately NOT cleared
    // here — see `pagingRefusedKind`'s doc comment for why (it would wipe
    // out on the very next tick, since queue fill is the only strategy an
    // out-of-range move can even happen under).
    const noLookRefusalResolved = this.pagingRefusedKind === "no-look" && resolution !== null;
    const fillRefusalResolved =
      this.pagingRefusedKind === "fill" && resolution !== null && resolution.boxFill === "queue";
    if (noLookRefusalResolved || fillRefusalResolved) {
      this.pagingRefused = null;
      this.pagingRefusedKind = null;
    }

    // Overlays: re-derive every tick and keep the change flag for Task 8.
    // No `ShowEngine` input sets `question` yet, so it is always `null`
    // today (see the field's own doc comment).
    this.overlaysChanged = this.overlayDirector.update({
      look: resolution,
      question: this.question,
      questionVisible: this.questionVisible
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
      await this.store.save(this.buildPersistedState());
      this.lastSaveTime = now;
      this.pendingPersist = false;
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

  /**
   * Mukana polling (Task 9, corrected in fix round 1): apply whatever
   * settled since the previous tick, then start any endpoint whose poll is
   * due AND not already in flight.
   *
   * Non-blocking by construction, never by discipline someone could get
   * wrong later: this method contains no `await` at all. Every fetch is
   * started and immediately `.then()`-attached without awaiting the
   * returned promise, so a hung registry can only ever delay when its OWN
   * outcome gets applied — never delay this or any other tick from
   * returning (spec §2, normative).
   *
   * `mukanaPollBusy[endpoint]` gates starting a NEW fetch while one is
   * still outstanding — see that field's own doc comment for what breaks
   * without it (unbounded concurrent requests to a hung endpoint, and
   * settle-order-not-start-order data landing stale-over-fresh). An earlier
   * revision of this method shipped WITHOUT the gate, reasoning that
   * settling a fetch takes several real microtask turns and each `tick()`
   * call is itself only one or two of those turns, so a busy gate could
   * leave an endpoint marked busy across ticks whose clock delta alone
   * would call it due. That reasoning was fitted to the test rig, not to
   * production: it measured true only because the ORIGINAL test rig never
   * let the microtask queue drain between `tick()` calls. In real time a
   * fetch settles in milliseconds against a multi-second polling interval —
   * millions of microtask turns of headroom — so the busy flag is cleared
   * long before the next poll is ever due. The rig now drains explicitly
   * (`flush()` in `showEngine.test.ts`) instead of the engine papering over
   * a fixture gap with a correctness gap of its own.
   *
   * A rejected fetch — a `FetchLike` that resolves `text()` to something
   * other than a string, or any other contract violation `MukanaClient`
   * doesn't already catch internally — must never become an unhandled
   * promise rejection (spec §2's whole point: a broken third-party
   * integration cannot be allowed to take the process down). Every
   * `.then()` below supplies BOTH handlers; the rejection handler clears
   * `mukanaPollBusy` exactly like the success path and records an
   * `invalid` outcome so a rejecting endpoint still surfaces as a normal
   * failure rather than silently going quiet or crashing the show.
   *
   * Endpoint gating mirrors `config.integrations`: an endpoint whose
   * integration is off is never polled, matching the rest of this
   * package's rule that an unconfigured integration must never silently
   * reach a URL nobody set (`config.ts`'s own `parseMukana` doc comment).
   */
  private pollMukana(client: MukanaClient): void {
    if (this.panelistsPollSettled !== null) {
      const outcome = this.panelistsPollSettled;
      this.panelistsPollSettled = null;
      this.onMukanaPanelists(outcome);
    }
    if (this.handsPollSettled !== null) {
      const outcome = this.handsPollSettled;
      this.handsPollSettled = null;
      this.applyHandsOutcome(outcome);
    }
    if (this.questionPollSettled !== null) {
      const outcome = this.questionPollSettled;
      this.questionPollSettled = null;
      this.applyQuestionOutcome(outcome);
    }

    const now = this.clock.now();

    if (
      this.config.integrations.registry &&
      !this.mukanaPollBusy.panelists &&
      this.isMukanaPollDue("panelists", now, client)
    ) {
      this.lastMukanaPollAt.panelists = now;
      this.mukanaPollBusy.panelists = true;
      client.fetchPanelists().then(
        (outcome) => {
          this.mukanaPollBusy.panelists = false;
          this.panelistsPollSettled = outcome;
        },
        (error: unknown) => {
          this.mukanaPollBusy.panelists = false;
          this.panelistsPollSettled = { kind: "invalid", reason: mukanaRejectionReason(error) };
        }
      );
    }
    if (
      this.config.integrations.handsQueue &&
      !this.mukanaPollBusy.hands &&
      this.isMukanaPollDue("hands", now, client)
    ) {
      this.lastMukanaPollAt.hands = now;
      this.mukanaPollBusy.hands = true;
      client.fetchHands().then(
        (outcome) => {
          this.mukanaPollBusy.hands = false;
          this.handsPollSettled = outcome;
        },
        (error: unknown) => {
          this.mukanaPollBusy.hands = false;
          this.handsPollSettled = { kind: "invalid", reason: mukanaRejectionReason(error) };
        }
      );
    }
    if (
      this.config.integrations.questionFeed &&
      !this.mukanaPollBusy.question &&
      this.isMukanaPollDue("question", now, client)
    ) {
      this.lastMukanaPollAt.question = now;
      this.mukanaPollBusy.question = true;
      client.fetchQuestion().then(
        (outcome) => {
          this.mukanaPollBusy.question = false;
          this.questionPollSettled = outcome;
        },
        (error: unknown) => {
          this.mukanaPollBusy.question = false;
          this.questionPollSettled = { kind: "invalid", reason: mukanaRejectionReason(error) };
        }
      );
    }
  }

  /** Whether `endpoint`'s next poll is due: `nextDelayMs` already folds interval + backoff; this adds no policy of its own. */
  private isMukanaPollDue(endpoint: MukanaEndpoint, now: number, client: MukanaClient): boolean {
    return now - this.lastMukanaPollAt[endpoint] >= client.nextDelayMs(endpoint);
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

  /** The current Mukana health snapshot, or the all-failing stand-in when there is no client at all. */
  private mukanaHealth(): Record<MukanaEndpoint, MukanaHealth> {
    return this.mukanaClient?.health ?? NO_REGISTRY_HEALTH;
  }

  /** Look up a `LookDefinition` by id, or `null` for a `null` id. Never throws — `setLook` is the validating gate. */
  private lookById(lookId: string | null): LookDefinition | null {
    if (lookId === null) return null;
    return this.config.looks.find((candidate) => candidate.id === lookId) ?? null;
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
