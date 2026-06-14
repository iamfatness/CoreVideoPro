/**
 * Teleprompter engine.
 * Manages a scrolling script for live presentation: segments the script into
 * lines, tracks scroll position and the active line, supports adjustable scroll
 * speed, and estimates read time. Pure TypeScript — no React, no timers.
 */

export type ScriptLine = {
  index: number;
  text: string;
  wordCount: number;
  /** Cumulative words from the start of the script through this line. */
  cumulativeWords: number;
};

export type TeleprompterState = {
  lines: ScriptLine[];
  totalWords: number;
  /** Words spoken per minute, used for read-time estimates. */
  wordsPerMinute: number;
  /** Current scroll offset measured in words from the start. */
  scrollWords: number;
  /** Scroll speed multiplier (1 = normal). */
  speed: number;
  /** Whether the prompter is actively scrolling. */
  running: boolean;
};

export type TeleprompterView = {
  activeLineIndex: number;
  activeLine: ScriptLine | null;
  /** 0–100 progress through the whole script. */
  progressPct: number;
  /** Words remaining after the current scroll position. */
  remainingWords: number;
  /** Estimated seconds remaining at the current WPM. */
  remainingSec: number;
  /** Estimated total read time in seconds. */
  totalReadSec: number;
  running: boolean;
};

const DEFAULT_WPM = 140;
const MIN_SPEED = 0.25;
const MAX_SPEED = 4;

function clamp(value: number, min: number, max: number): number {
  return Math.max(min, Math.min(max, value));
}

function countWords(text: string): number {
  const trimmed = text.trim();
  if (trimmed.length === 0) return 0;
  return trimmed.split(/\s+/).length;
}

/**
 * Parse a raw script into non-empty lines with cumulative word counts.
 */
export function parseScript(script: string): ScriptLine[] {
  const rawLines = script.split("\n").map((l) => l.trim()).filter((l) => l.length > 0);
  let cumulative = 0;
  return rawLines.map((text, index) => {
    const wordCount = countWords(text);
    cumulative += wordCount;
    return { index, text, wordCount, cumulativeWords: cumulative };
  });
}

/**
 * Create a teleprompter from a raw script string.
 */
export function createTeleprompter(script: string, wordsPerMinute = DEFAULT_WPM): TeleprompterState {
  const lines = parseScript(script);
  const totalWords = lines.reduce((sum, l) => sum + l.wordCount, 0);
  return {
    lines,
    totalWords,
    wordsPerMinute: Math.max(1, wordsPerMinute),
    scrollWords: 0,
    speed: 1,
    running: false,
  };
}

/**
 * Start scrolling.
 */
export function startScroll(state: TeleprompterState): TeleprompterState {
  return { ...state, running: true };
}

/**
 * Pause scrolling.
 */
export function pauseScroll(state: TeleprompterState): TeleprompterState {
  return { ...state, running: false };
}

/**
 * Reset scroll position to the start (does not change running state).
 */
export function resetScroll(state: TeleprompterState): TeleprompterState {
  return { ...state, scrollWords: 0 };
}

/**
 * Set the scroll speed multiplier (clamped to [0.25, 4]).
 */
export function setSpeed(state: TeleprompterState, speed: number): TeleprompterState {
  return { ...state, speed: clamp(speed, MIN_SPEED, MAX_SPEED) };
}

/**
 * Set words-per-minute (minimum 1).
 */
export function setWordsPerMinute(state: TeleprompterState, wpm: number): TeleprompterState {
  return { ...state, wordsPerMinute: Math.max(1, wpm) };
}

/**
 * Advance the scroll position by deltaMs. When not running, position is
 * unchanged. Scroll rate is wpm * speed, in words per minute.
 */
export function advanceScroll(state: TeleprompterState, deltaMs: number): TeleprompterState {
  if (!state.running || deltaMs <= 0) return state;
  const wordsPerMs = (state.wordsPerMinute * state.speed) / 60000;
  const next = state.scrollWords + wordsPerMs * deltaMs;
  const clamped = clamp(next, 0, state.totalWords);
  const running = clamped < state.totalWords;
  return { ...state, scrollWords: clamped, running };
}

/**
 * Jump scroll position to the start of a specific line.
 */
export function jumpToLine(state: TeleprompterState, lineIndex: number): TeleprompterState {
  const line = state.lines[lineIndex];
  if (!line) return state;
  const startWords = line.cumulativeWords - line.wordCount;
  return { ...state, scrollWords: clamp(startWords, 0, state.totalWords) };
}

/**
 * The index of the line currently under the scroll position.
 */
export function activeLineIndex(state: TeleprompterState): number {
  if (state.lines.length === 0) return -1;
  for (const line of state.lines) {
    if (state.scrollWords < line.cumulativeWords) {
      return line.index;
    }
  }
  return state.lines.length - 1;
}

/**
 * Compute a display view: active line, progress, and read-time estimates.
 */
export function teleprompterView(state: TeleprompterState): TeleprompterView {
  const idx = activeLineIndex(state);
  const activeLine = idx >= 0 ? state.lines[idx] : null;
  const progressPct = state.totalWords > 0
    ? clamp((state.scrollWords / state.totalWords) * 100, 0, 100)
    : 0;
  const remainingWords = Math.max(0, state.totalWords - state.scrollWords);
  const remainingSec = Math.round((remainingWords / state.wordsPerMinute) * 60);
  const totalReadSec = Math.round((state.totalWords / state.wordsPerMinute) * 60);
  return {
    activeLineIndex: idx,
    activeLine,
    progressPct,
    remainingWords,
    remainingSec,
    totalReadSec,
    running: state.running,
  };
}

/**
 * Format a seconds duration as M:SS.
 */
export function formatReadTime(sec: number): string {
  const safe = Math.max(0, Math.round(sec));
  const m = Math.floor(safe / 60);
  const s = safe % 60;
  return `${m}:${String(s).padStart(2, "0")}`;
}
