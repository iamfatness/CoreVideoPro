/**
 * Tally light engine.
 * Tracks program/preview tally state for each source (participant, screen-share, graphic).
 * Supports both hardware tally (NDI, GPI) and software tally display.
 */

export type TallyState = "program" | "preview" | "idle";

export type TallySource = {
  sourceId: string;
  label: string;
  /** Current tally state on this source */
  tally: TallyState;
};

export type TallySnapshot = {
  sources: TallySource[];
  programSourceIds: string[];
  previewSourceIds: string[];
  /** Sources currently on air */
  onAirCount: number;
  summary: string;
};

export type TallyColor = {
  r: number;
  g: number;
  b: number;
};

/** Standard broadcast tally colors */
export const TALLY_COLORS: Record<TallyState, TallyColor> = {
  program: { r: 255, g: 0, b: 0 },   // Red = on air
  preview: { r: 0, g: 255, b: 0 },   // Green = in preview
  idle: { r: 0, g: 0, b: 0 },        // Off
};

export type TallyInput = {
  sourceId: string;
  label: string;
};

/**
 * Compute tally state for all sources given the current program and preview scene slot assignments.
 */
export function computeTally(
  sources: TallyInput[],
  programSourceIds: string[],
  previewSourceIds: string[]
): TallySnapshot {
  const programSet = new Set(programSourceIds);
  const previewSet = new Set(previewSourceIds);

  const tallyedSources: TallySource[] = sources.map((source) => {
    let tally: TallyState = "idle";
    if (programSet.has(source.sourceId)) {
      tally = "program";
    } else if (previewSet.has(source.sourceId)) {
      tally = "preview";
    }
    return { sourceId: source.sourceId, label: source.label, tally };
  });

  const onAirCount = tallyedSources.filter((s) => s.tally === "program").length;

  return {
    sources: tallyedSources,
    programSourceIds,
    previewSourceIds,
    onAirCount,
    summary: buildTallySummary(tallyedSources, onAirCount),
  };
}

/**
 * Get the tally color for a given state (for hardware tally drivers).
 */
export function tallyColor(state: TallyState): TallyColor {
  return TALLY_COLORS[state];
}

/**
 * Format tally color as a CSS hex string.
 */
export function tallyColorHex(state: TallyState): string {
  const { r, g, b } = TALLY_COLORS[state];
  return `#${r.toString(16).padStart(2, "0")}${g.toString(16).padStart(2, "0")}${b.toString(16).padStart(2, "0")}`;
}

/**
 * Return only sources matching a specific tally state.
 */
export function filterByTally(snapshot: TallySnapshot, state: TallyState): TallySource[] {
  return snapshot.sources.filter((s) => s.tally === state);
}

/**
 * Check whether a specific source is currently on program.
 */
export function isOnProgram(snapshot: TallySnapshot, sourceId: string): boolean {
  return snapshot.programSourceIds.includes(sourceId);
}

/**
 * Check whether a specific source is currently in preview.
 */
export function isOnPreview(snapshot: TallySnapshot, sourceId: string): boolean {
  return snapshot.previewSourceIds.includes(sourceId);
}

/**
 * Produce a tally report suitable for sending to hardware tally controllers.
 * Returns a flat map of sourceId → tally state string.
 */
export function exportTallyReport(snapshot: TallySnapshot): Record<string, TallyState> {
  return Object.fromEntries(snapshot.sources.map((s) => [s.sourceId, s.tally]));
}

/**
 * Merge two tally snapshots — useful when compositing multiple video layers.
 * Program wins over preview; preview wins over idle.
 */
export function mergeTallySnapshots(a: TallySnapshot, b: TallySnapshot): TallySnapshot {
  const allSources = new Map<string, TallyInput>();
  for (const s of [...a.sources, ...b.sources]) {
    allSources.set(s.sourceId, { sourceId: s.sourceId, label: s.label });
  }

  const programIds = Array.from(new Set([...a.programSourceIds, ...b.programSourceIds]));
  const previewIds = Array.from(
    new Set([...a.previewSourceIds, ...b.previewSourceIds])
  ).filter((id) => !programIds.includes(id));

  return computeTally(Array.from(allSources.values()), programIds, previewIds);
}

function buildTallySummary(sources: TallySource[], onAirCount: number): string {
  if (sources.length === 0) return "No sources";
  if (onAirCount === 0) return "All sources idle";
  const onAirLabels = sources.filter((s) => s.tally === "program").map((s) => s.label);
  return `On air: ${onAirLabels.join(", ")}`;
}
