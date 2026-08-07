/**
 * The show engine — the class that owns every module instance and is the
 * first real composition in this package. Twenty-one modules were shipped
 * individually tested but never wired together; this is that wiring.
 *
 * This file covers construction, restore-from-disk, and the snapshot
 * accessor only. There is no tick yet (Task 5), no speaker gate wiring
 * (Task 6), no host command emission (Task 7+), and no polling loop
 * (Task 8+) — `setLook` is implemented here only far enough to select the
 * active look id, because two restore tests need it; its tick behavior
 * (recomputing the resolved look, clamping the page, clearing manual boxes
 * on a look change) is Task 6/7's job.
 */

import type { Clock } from "./clock.js";
import type { HostAdapter } from "./hostAdapter.js";
import type { PositionAssigner } from "./speakerRecency.js";
import { FiloAssigner } from "./speakerRecency.js";
import type { ShowEngineConfig } from "./config.js";
import type { MukanaClient, MukanaEndpoint, MukanaHealth } from "./mukanaClient.js";
import { MukanaRegistry } from "./mukanaParse.js";
import { LiveSlots } from "./liveSlots.js";
import { GalleryDirector } from "./galleryDirector.js";
import { OverrideDb } from "./overrideDb.js";
import { ZoomIngest } from "./zoomIngest.js";
import { ProgramBus } from "./programBus.js";
import { OverlayDirector } from "./overlayDirector.js";
import { buildPanelistDb } from "./panelistDb.js";
import { deriveTally } from "./tallyPublisher.js";
import { resolveCapabilities } from "./capabilities.js";
import { buildSnapshot, type ShowSnapshot } from "./showSnapshot.js";
import { StateStore } from "./persistence.js";
import type { LookResolution, ManualBoxAssignments } from "./lookDirector.js";
import type { QueueState } from "./contracts.js";

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

  /** The tick counter, starting at 0. Advanced by `tick()` in a later task. */
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
      health
    });
  }
}
