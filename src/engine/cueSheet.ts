/**
 * Cue sheet engine for live show production.
 * Manages ordered cues with timing, status transitions, and progress tracking.
 * Pure TypeScript — no React, no side effects.
 */

export type CueType = "intro" | "segment" | "break" | "qa" | "outro" | "ad-break" | "bumper";
export type CueStatus = "pending" | "ready" | "live" | "done" | "skipped";

export type Cue = {
  id: string;
  order: number;           // 1-based display order
  type: CueType;
  title: string;
  notes: string;
  plannedDurationSec: number;
  actualStartSec: number | null;  // null = not started
  actualEndSec: number | null;    // null = not ended
  status: CueStatus;
};

export type CueSheet = {
  id: string;
  showTitle: string;
  cues: Cue[];
  activeCueId: string | null;
};

export type CueProgress = {
  cue: Cue;
  elapsedSec: number;
  remainingSec: number;
  overrunSec: number;       // > 0 when elapsed > planned
  pct: number;              // 0-100 (clamped)
  overrun: boolean;
};

export type CueSheetSummary = {
  totalPlannedSec: number;
  totalActualSec: number;
  doneCount: number;
  pendingCount: number;
  overrunSec: number;       // sum of overruns across done cues
  estimatedEndSec: number;  // nowSec + remaining planned time
  summary: string;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

let _idCounter = 0;

function generateId(prefix = "cue"): string {
  _idCounter += 1;
  return `${prefix}-${Date.now()}-${_idCounter}`;
}

function renumber(cues: Cue[]): Cue[] {
  return cues.map((c, i) => ({ ...c, order: i + 1 }));
}

// ---------------------------------------------------------------------------
// createCueSheet
// ---------------------------------------------------------------------------

export function createCueSheet(showTitle: string, cues?: Partial<Cue>[]): CueSheet {
  const sheetId = generateId("sheet");

  const resolvedCues: Cue[] = (cues ?? []).map((partial, index) => ({
    id: partial.id ?? generateId(),
    order: index + 1,
    type: partial.type ?? "segment",
    title: partial.title ?? "",
    notes: partial.notes ?? "",
    plannedDurationSec: partial.plannedDurationSec ?? 0,
    actualStartSec: partial.actualStartSec ?? null,
    actualEndSec: partial.actualEndSec ?? null,
    status: partial.status ?? "pending",
  }));

  return {
    id: sheetId,
    showTitle,
    cues: resolvedCues,
    activeCueId: null,
  };
}

// ---------------------------------------------------------------------------
// addCue
// ---------------------------------------------------------------------------

export function addCue(
  sheet: CueSheet,
  type: CueType,
  title: string,
  durationSec: number,
  notes = ""
): CueSheet {
  const nextOrder = sheet.cues.length + 1;
  const newCue: Cue = {
    id: generateId(),
    order: nextOrder,
    type,
    title,
    notes,
    plannedDurationSec: durationSec,
    actualStartSec: null,
    actualEndSec: null,
    status: "pending",
  };
  return { ...sheet, cues: [...sheet.cues, newCue] };
}

// ---------------------------------------------------------------------------
// removeCue
// ---------------------------------------------------------------------------

export function removeCue(sheet: CueSheet, cueId: string): CueSheet {
  const filtered = sheet.cues.filter((c) => c.id !== cueId);
  const renumbered = renumber(filtered);
  const activeCueId = sheet.activeCueId === cueId ? null : sheet.activeCueId;
  return { ...sheet, cues: renumbered, activeCueId };
}

// ---------------------------------------------------------------------------
// reorderCue
// ---------------------------------------------------------------------------

export function reorderCue(sheet: CueSheet, cueId: string, newOrder: number): CueSheet {
  const cues = [...sheet.cues];
  const fromIndex = cues.findIndex((c) => c.id === cueId);
  if (fromIndex === -1) return sheet;

  // Clamp newOrder to valid range [1, cues.length]
  const clampedOrder = Math.max(1, Math.min(newOrder, cues.length));
  const toIndex = clampedOrder - 1;

  const [moved] = cues.splice(fromIndex, 1);
  cues.splice(toIndex, 0, moved);

  return { ...sheet, cues: renumber(cues) };
}

// ---------------------------------------------------------------------------
// goLiveCue
// ---------------------------------------------------------------------------

export function goLiveCue(sheet: CueSheet, cueId: string, nowSec: number): CueSheet {
  const cues = sheet.cues.map((c) => {
    if (c.id === sheet.activeCueId && c.status === "live") {
      // Auto-end the previously live cue
      return { ...c, status: "done" as CueStatus, actualEndSec: nowSec };
    }
    if (c.id === cueId) {
      return {
        ...c,
        status: "live" as CueStatus,
        actualStartSec: nowSec,
        actualEndSec: null,
      };
    }
    return c;
  });

  return { ...sheet, cues, activeCueId: cueId };
}

// ---------------------------------------------------------------------------
// endCue
// ---------------------------------------------------------------------------

export function endCue(sheet: CueSheet, cueId: string, nowSec: number): CueSheet {
  const cues = sheet.cues.map((c) => {
    if (c.id === cueId) {
      return { ...c, status: "done" as CueStatus, actualEndSec: nowSec };
    }
    return c;
  });

  const activeCueId = sheet.activeCueId === cueId ? null : sheet.activeCueId;
  return { ...sheet, cues, activeCueId };
}

// ---------------------------------------------------------------------------
// skipCue
// ---------------------------------------------------------------------------

export function skipCue(sheet: CueSheet, cueId: string): CueSheet {
  const cues = sheet.cues.map((c) => {
    if (c.id === cueId && (c.status === "pending" || c.status === "ready")) {
      return { ...c, status: "skipped" as CueStatus };
    }
    return c;
  });
  return { ...sheet, cues };
}

// ---------------------------------------------------------------------------
// nextCue
// ---------------------------------------------------------------------------

export function nextCue(sheet: CueSheet): Cue | null {
  const available = sheet.cues.filter(
    (c) => c.status === "pending" || c.status === "ready"
  );
  if (available.length === 0) return null;

  if (sheet.activeCueId == null) {
    return available[0];
  }

  const activeIndex = sheet.cues.findIndex((c) => c.id === sheet.activeCueId);
  // Find first pending/ready cue with a higher order than the active cue
  const after = available.filter((c) => {
    const idx = sheet.cues.findIndex((x) => x.id === c.id);
    return idx > activeIndex;
  });

  return after.length > 0 ? after[0] : available[0];
}

// ---------------------------------------------------------------------------
// computeCueProgress
// ---------------------------------------------------------------------------

export function computeCueProgress(cue: Cue, nowSec: number): CueProgress {
  const startSec = cue.actualStartSec ?? nowSec;
  const elapsedSec = nowSec - startSec;
  const remainingSec = cue.plannedDurationSec - elapsedSec;
  const overrunSec = Math.max(0, elapsedSec - cue.plannedDurationSec);
  const pct = cue.plannedDurationSec > 0
    ? Math.min(100, Math.max(0, (elapsedSec / cue.plannedDurationSec) * 100))
    : 0;
  const overrun = overrunSec > 0;

  return { cue, elapsedSec, remainingSec, overrunSec, pct, overrun };
}

// ---------------------------------------------------------------------------
// summarizeCueSheet
// ---------------------------------------------------------------------------

export function summarizeCueSheet(sheet: CueSheet, nowSec: number): CueSheetSummary {
  const totalPlannedSec = sheet.cues.reduce((sum, c) => sum + c.plannedDurationSec, 0);

  let totalActualSec = 0;
  let doneCount = 0;
  let pendingCount = 0;
  let overrunSec = 0;

  for (const c of sheet.cues) {
    if (c.status === "done") {
      doneCount++;
      const start = c.actualStartSec ?? 0;
      const end = c.actualEndSec ?? nowSec;
      const actualDur = end - start;
      totalActualSec += actualDur;
      const over = actualDur - c.plannedDurationSec;
      if (over > 0) overrunSec += over;
    } else if (c.status === "live") {
      const start = c.actualStartSec ?? nowSec;
      totalActualSec += nowSec - start;
    } else if (c.status === "pending" || c.status === "ready") {
      pendingCount++;
    }
  }

  // Remaining planned time = sum of planned durations for pending/ready cues
  const remainingPlannedSec = sheet.cues
    .filter((c) => c.status === "pending" || c.status === "ready")
    .reduce((sum, c) => sum + c.plannedDurationSec, 0);

  const estimatedEndSec = nowSec + remainingPlannedSec;

  const summary =
    `${doneCount} done, ${pendingCount} pending — ` +
    `planned ${totalPlannedSec}s, actual ${Math.round(totalActualSec)}s` +
    (overrunSec > 0 ? `, overrun ${Math.round(overrunSec)}s` : "");

  return {
    totalPlannedSec,
    totalActualSec,
    doneCount,
    pendingCount,
    overrunSec,
    estimatedEndSec,
    summary,
  };
}

// ---------------------------------------------------------------------------
// exportCueSheet
// ---------------------------------------------------------------------------

export function exportCueSheet(sheet: CueSheet, format: "text" | "json" | "csv"): string {
  if (format === "json") {
    return JSON.stringify(sheet, null, 2);
  }

  if (format === "csv") {
    const header = "order,type,title,planned,status";
    const rows = sheet.cues.map(
      (c) => `${c.order},${c.type},"${c.title.replace(/"/g, '""')}",${c.plannedDurationSec},${c.status}`
    );
    return [header, ...rows].join("\n");
  }

  // text format: human-readable rundown
  const lines: string[] = [`CUE SHEET: ${sheet.showTitle}`, ""];
  for (const c of sheet.cues) {
    const label = cueTypeLabel(c.type);
    const mins = Math.floor(c.plannedDurationSec / 60);
    const secs = c.plannedDurationSec % 60;
    const dur = `${mins}:${String(secs).padStart(2, "0")}`;
    const statusMark =
      c.status === "live"
        ? "[LIVE]"
        : c.status === "done"
        ? "[DONE]"
        : c.status === "skipped"
        ? "[SKIP]"
        : c.status === "ready"
        ? "[RDY]"
        : "[ ]";
    lines.push(`${String(c.order).padStart(2, " ")}. ${statusMark} ${label.padEnd(10)} ${dur}  ${c.title}`);
    if (c.notes) {
      lines.push(`        Notes: ${c.notes}`);
    }
  }
  return lines.join("\n");
}

// ---------------------------------------------------------------------------
// cueTypeLabel
// ---------------------------------------------------------------------------

export function cueTypeLabel(type: CueType): string {
  switch (type) {
    case "intro":     return "Intro";
    case "segment":   return "Segment";
    case "break":     return "Break";
    case "qa":        return "Q&A";
    case "outro":     return "Outro";
    case "ad-break":  return "Ad Break";
    case "bumper":    return "Bumper";
  }
}
