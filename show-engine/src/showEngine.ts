/**
 * The show engine — the class that owns every module instance and is the
 * first real composition in this package. Twenty-one modules were shipped
 * individually tested but never wired together; this is that wiring.
 *
 * This file covers construction, restore-from-disk, the snapshot accessor,
 * roster intake, and the seating tick (Task 5). There is no speaker gate
 * wiring (Task 6), no derived-layer recompute or host command emission
 * (Task 7/8+), and no polling loop (Task 9+) — `setLook` is implemented
 * here only far enough to select the active look id, because two restore
 * tests need it; its tick behavior (recomputing the resolved look, clamping
 * the page, clearing manual boxes on a look change) is Task 6/7's job.
 */

import type { Clock } from "./clock.js";
import type { HostAdapter } from "./hostAdapter.js";
import type { PositionAssigner } from "./speakerRecency.js";
import { FiloAssigner } from "./speakerRecency.js";
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
import type { LookResolution, ManualBoxAssignments } from "./lookDirector.js";
import { EXCLUSIVE_ROLES, type Panelist, type QueueState, type Role } from "./contracts.js";
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
   * Select the active look by id. This is a minimal setter for now — it
   * only records which look definition is active. Task 6 gives it its tick
   * behavior (resolving it against the live roster, clamping the page,
   * clearing manual boxes on a change). Throws on an unknown look id.
   */
  setLook(lookId: string): void {
    const look = this.config.looks.find((candidate) => candidate.id === lookId);
    if (look === undefined) {
      throw new Error(`ShowEngine.setLook: unknown look id ${JSON.stringify(lookId)}`);
    }
    this.selectedLookId = lookId;
    this.pendingPersist = true;
    this.otherInputsChanged = true;
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
   * only when it actually changed), recompute the derived layers (Tasks
   * 6–7 — a no-op today), persist if the debounce window allows it, emit
   * host commands (Task 8 — a no-op today), then advance the revision and
   * publish.
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
    const rosterInputsChanged = rosterCommitted || otherChanged;

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
        // reseat the room.
        this.liveSlots.refresh(panelists);
      } else {
        // The roster itself changed (a join, or the very first tick, when
        // there is no prior arrangement to hold). Seat deterministically by
        // participant id — sorting by name would reshuffle the room
        // whenever someone renamed themselves mid-show — and never drop
        // whoever doesn't fit: `rebuild`'s overflow return is what the
        // published snapshot's `unseated` reports.
        const sorted = [...panelists.values()].sort((a, b) =>
          a.participantId < b.participantId ? -1 : a.participantId > b.participantId ? 1 : 0
        );
        this.unseatedPanelists = this.liveSlots.rebuild(sorted);
      }
      this.lastSeatedParticipantIds = currentIds;

      // A participant who has gone offline (a Zoom "left", or a roster
      // snapshot that no longer reports them present) vacates their seat
      // instead of sitting in it forever as an offline ghost. `refresh`
      // itself only marks a vanished-from-the-database seat offline and
      // holds it (LiveSlots' documented behavior) — this is the layer that
      // turns a genuine departure into the open hole an operator expects,
      // without touching anyone else's seat.
      for (const slot of this.liveSlots.slots()) {
        if (slot.panelist !== null && !slot.panelist.online) {
          this.liveSlots.removeSlot(slot.slot);
        }
      }

      this.pendingPersist = true;
    }

    // Derived layers (look resolution, tally recompute, overlays) land in
    // Tasks 6–7; host command emission lands in Task 8. Nothing to do here
    // yet — `snapshot()` already recomputes tally/capabilities live from
    // current module state.

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
    const health = this.mukanaClient?.health ?? NO_REGISTRY_HEALTH;
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
      unseated: this.unseatedPanelists
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
}
