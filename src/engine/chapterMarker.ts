/**
 * Chapter marker engine.
 * Manages timestamped chapter markers on the output recording,
 * produces export formats for YouTube, VTT, and plain text.
 */

export type ChapterMarker = {
  id: string;
  /** Offset from recording start in seconds */
  startSec: number;
  title: string;
  description?: string;
};

export type ChapterList = {
  markers: ChapterMarker[];
  /** Total recording duration in seconds (used for end-time calculation) */
  recordingDurationSec: number;
};

export type ChapterExportFormat = "youtube" | "vtt" | "text" | "json";

/** Minimum gap between chapters (YouTube requires at least 10 seconds) */
const MIN_CHAPTER_GAP_SEC = 10;
/** YouTube requires the first chapter to start at 00:00 */
const YOUTUBE_FIRST_CHAPTER_SEC = 0;

let _idCounter = 0;
function makeId(): string {
  return `ch-${++_idCounter}-${Math.random().toString(36).slice(2, 6)}`;
}

/**
 * Add a chapter marker, keeping the list sorted by startSec.
 */
export function addChapter(
  list: ChapterList,
  startSec: number,
  title: string,
  description?: string
): ChapterList {
  const marker: ChapterMarker = {
    id: makeId(),
    startSec: Math.max(0, startSec),
    title: title.trim() || "Chapter",
    description,
  };
  const markers = [...list.markers, marker].sort((a, b) => a.startSec - b.startSec);
  return { ...list, markers };
}

/**
 * Remove a chapter marker by id.
 */
export function removeChapter(list: ChapterList, id: string): ChapterList {
  return { ...list, markers: list.markers.filter((m) => m.id !== id) };
}

/**
 * Update a chapter's title or startSec.
 */
export function updateChapter(
  list: ChapterList,
  id: string,
  update: Partial<Pick<ChapterMarker, "title" | "startSec" | "description">>
): ChapterList {
  const markers = list.markers
    .map((m) => (m.id === id ? { ...m, ...update } : m))
    .sort((a, b) => a.startSec - b.startSec);
  return { ...list, markers };
}

/**
 * Validate the chapter list for export platform requirements.
 */
export function validateChapters(
  list: ChapterList,
  platform: "youtube" | "generic" = "generic"
): string[] {
  const warnings: string[] = [];

  if (list.markers.length === 0) {
    return ["No chapters defined"];
  }

  if (platform === "youtube") {
    if (list.markers[0].startSec !== YOUTUBE_FIRST_CHAPTER_SEC) {
      warnings.push("YouTube requires first chapter to start at 00:00");
    }
    if (list.markers.length < 3) {
      warnings.push("YouTube requires at least 3 chapters to display chapter markers");
    }
  }

  for (let i = 1; i < list.markers.length; i++) {
    const gap = list.markers[i].startSec - list.markers[i - 1].startSec;
    if (gap < MIN_CHAPTER_GAP_SEC) {
      warnings.push(
        `"${list.markers[i].title}" is only ${gap.toFixed(1)}s after previous chapter (min ${MIN_CHAPTER_GAP_SEC}s)`
      );
    }
  }

  return warnings;
}

/**
 * Format a chapter start time as HH:MM:SS.
 */
export function formatChapterTime(seconds: number): string {
  const s = Math.floor(Math.max(0, seconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0) {
    return `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}:${String(sec).padStart(2, "0")}`;
  }
  return `${String(m).padStart(2, "0")}:${String(sec).padStart(2, "0")}`;
}

/**
 * Export the chapter list to the requested format string.
 */
export function exportChapters(list: ChapterList, format: ChapterExportFormat): string {
  if (list.markers.length === 0) return "";

  switch (format) {
    case "youtube":
      return list.markers
        .map((m) => `${formatChapterTime(m.startSec)} ${m.title}`)
        .join("\n");

    case "vtt": {
      const lines = ["WEBVTT", ""];
      for (let i = 0; i < list.markers.length; i++) {
        const start = list.markers[i].startSec;
        const end = list.markers[i + 1]?.startSec ?? list.recordingDurationSec;
        lines.push(`${vttTime(start)} --> ${vttTime(end)}`);
        lines.push(list.markers[i].title);
        lines.push("");
      }
      return lines.join("\n");
    }

    case "json":
      return JSON.stringify(list.markers, null, 2);

    case "text":
    default:
      return list.markers
        .map((m, i) => `${i + 1}. [${formatChapterTime(m.startSec)}] ${m.title}${m.description ? ` — ${m.description}` : ""}`)
        .join("\n");
  }
}

/**
 * Find which chapter is active at a given playback position.
 */
export function activeChapterAt(list: ChapterList, positionSec: number): ChapterMarker | null {
  let active: ChapterMarker | null = null;
  for (const marker of list.markers) {
    if (marker.startSec <= positionSec) {
      active = marker;
    } else {
      break;
    }
  }
  return active;
}

/**
 * Auto-generate chapters from the show rundown scene names and their elapsed times.
 */
export function chaptersFromRundown(
  segments: Array<{ name: string; startSec: number }>
): ChapterList {
  const totalSec = segments.length > 0
    ? segments[segments.length - 1].startSec + 600
    : 0;

  const list: ChapterList = { markers: [], recordingDurationSec: totalSec };
  return segments.reduce(
    (acc, seg) => addChapter(acc, seg.startSec, seg.name),
    list
  );
}

export function summarizeChapters(list: ChapterList): string {
  if (list.markers.length === 0) return "No chapters";
  return `${list.markers.length} chapter${list.markers.length !== 1 ? "s" : ""}, ${formatChapterTime(list.recordingDurationSec)} total`;
}

function vttTime(seconds: number): string {
  const s = Math.max(0, seconds);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}:${sec.toFixed(3).padStart(6, "0")}`;
}
