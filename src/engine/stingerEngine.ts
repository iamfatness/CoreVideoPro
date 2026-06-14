/**
 * Stinger animation engine.
 * Plans and advances branded stinger transitions: in-phase, hold (scene-switch
 * point), and out-phase. Companion to transitions.ts which handles the simpler
 * cut/fade/slide/wipe styles.
 */

/** When during the stinger the scene switch (cut to new scene) should happen */
export type SceneSwitchPoint = "on-hold-start" | "on-hold-midpoint" | "on-out-start";

export type StingerPhase = "idle" | "in" | "hold" | "out" | "complete";

export type StingerAsset = {
  id: string;
  name: string;
  /** Duration of the wipe-in animation (ms) */
  inDurationMs: number;
  /** Duration the stinger covers the full frame before revealing new scene (ms) */
  holdDurationMs: number;
  /** Duration of the wipe-out animation revealing new scene (ms) */
  outDurationMs: number;
  /** When in the lifecycle to cut to the new scene */
  sceneSwitchPoint: SceneSwitchPoint;
};

export type StingerPlayback = {
  asset: StingerAsset;
  phase: StingerPhase;
  /** Elapsed ms since stinger started */
  elapsedMs: number;
  /** Absolute ms offset when the scene switch should occur */
  sceneSwitchAtMs: number;
  /** Total stinger duration */
  totalDurationMs: number;
  /** Whether the scene switch has already been triggered */
  sceneSwitched: boolean;
};

const BUILT_IN_STINGERS: StingerAsset[] = [
  {
    id: "brand-wipe",
    name: "Brand Wipe",
    inDurationMs: 300,
    holdDurationMs: 200,
    outDurationMs: 300,
    sceneSwitchPoint: "on-hold-start",
  },
  {
    id: "logo-reveal",
    name: "Logo Reveal",
    inDurationMs: 400,
    holdDurationMs: 400,
    outDurationMs: 400,
    sceneSwitchPoint: "on-hold-midpoint",
  },
  {
    id: "news-flash",
    name: "News Flash",
    inDurationMs: 150,
    holdDurationMs: 100,
    outDurationMs: 150,
    sceneSwitchPoint: "on-hold-start",
  },
  {
    id: "smooth-dissolve",
    name: "Smooth Dissolve",
    inDurationMs: 500,
    holdDurationMs: 0,
    outDurationMs: 500,
    sceneSwitchPoint: "on-out-start",
  },
];

export function stingerPresets(): StingerAsset[] {
  return BUILT_IN_STINGERS;
}

export function getStingerPreset(id: string): StingerAsset | undefined {
  return BUILT_IN_STINGERS.find((s) => s.id === id);
}

export function stingerTotalDurationMs(asset: StingerAsset): number {
  return asset.inDurationMs + asset.holdDurationMs + asset.outDurationMs;
}

/**
 * Calculate the absolute ms offset at which the scene switch should fire.
 */
export function stingerSceneSwitchMs(asset: StingerAsset): number {
  switch (asset.sceneSwitchPoint) {
    case "on-hold-start":
      return asset.inDurationMs;
    case "on-hold-midpoint":
      return asset.inDurationMs + Math.floor(asset.holdDurationMs / 2);
    case "on-out-start":
      return asset.inDurationMs + asset.holdDurationMs;
  }
}

/**
 * Determine which phase of the stinger is active at the given elapsed time.
 */
export function stingerPhaseAt(asset: StingerAsset, elapsedMs: number): StingerPhase {
  const total = stingerTotalDurationMs(asset);
  if (elapsedMs <= 0) return "idle";
  if (elapsedMs >= total) return "complete";
  if (elapsedMs < asset.inDurationMs) return "in";
  if (elapsedMs < asset.inDurationMs + asset.holdDurationMs) return "hold";
  return "out";
}

/** Create a new stinger playback state at t=0 */
export function startStinger(asset: StingerAsset): StingerPlayback {
  return {
    asset,
    phase: "in",
    elapsedMs: 0,
    sceneSwitchAtMs: stingerSceneSwitchMs(asset),
    totalDurationMs: stingerTotalDurationMs(asset),
    sceneSwitched: stingerSceneSwitchMs(asset) === 0,
  };
}

/**
 * Advance the stinger by deltaMs and return the updated playback state.
 * The caller should check `sceneSwitched` transitioning from false → true
 * to trigger the actual scene cut.
 */
export function advanceStinger(playback: StingerPlayback, deltaMs: number): StingerPlayback {
  const elapsedMs = Math.min(
    playback.elapsedMs + deltaMs,
    playback.totalDurationMs
  );
  const phase = stingerPhaseAt(playback.asset, elapsedMs);
  const sceneSwitched =
    playback.sceneSwitched || elapsedMs >= playback.sceneSwitchAtMs;

  return { ...playback, elapsedMs, phase, sceneSwitched };
}

export function isStingerComplete(playback: StingerPlayback): boolean {
  return playback.phase === "complete";
}

/**
 * Fractional progress within the current phase (0–1), useful for
 * driving CSS animation progress.
 */
export function stingerPhaseProgress(playback: StingerPlayback): number {
  const { asset, elapsedMs, phase } = playback;
  switch (phase) {
    case "in":
      return asset.inDurationMs > 0 ? elapsedMs / asset.inDurationMs : 1;
    case "hold": {
      const holdElapsed = elapsedMs - asset.inDurationMs;
      return asset.holdDurationMs > 0 ? holdElapsed / asset.holdDurationMs : 1;
    }
    case "out": {
      const outElapsed = elapsedMs - asset.inDurationMs - asset.holdDurationMs;
      return asset.outDurationMs > 0 ? outElapsed / asset.outDurationMs : 1;
    }
    case "complete":
      return 1;
    default:
      return 0;
  }
}

export function describeStinger(asset: StingerAsset): string {
  const total = stingerTotalDurationMs(asset);
  const switchMs = stingerSceneSwitchMs(asset);
  return `${asset.name} — ${total}ms total, scene switch at ${switchMs}ms`;
}
