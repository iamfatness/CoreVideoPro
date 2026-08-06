/**
 * Hands-raised queue parsing and chair stripping.
 * The hands endpoint reports who is waiting to ask a question as a legacy
 * three-line text payload: upcoming PINs, the current speaker's PIN, then
 * previously-shown PINs, with the sentinel `NONE` standing in for an empty
 * list or absent current speaker. The host and reader occupy dedicated boxes
 * in every on-screen look that includes them, so `stripChairs` removes them
 * from the guest queue to avoid double-booking. `queueOrder` derives the
 * display order the rest of the engine renders from.
 */

import type { QueueState } from "./contracts.js";

export type HandsOutcome =
  | { kind: "data"; queue: QueueState }
  | { kind: "invalid"; reason: string };

const PIN_PATTERN = /^\d{4}$/;

/**
 * Validate one payload line: it must be either the sentinel `NONE`
 * (case-sensitive, checked against the whole trimmed line) or a
 * comma-separated list whose every entry, after trimming, is exactly four
 * digits. Anything else — an HTML error page, a captive-portal splash, a
 * PHP warning block, a stray non-numeric token — is not a queue the engine
 * can silently treat as empty, so it is reported as unparseable rather than
 * dropped. Returns the validated entries (empty for `NONE`), or `null` when
 * the line does not conform to either shape.
 */
function parseLine(raw: string): string[] | null {
  if (raw.trim() === "NONE") return [];
  const entries = raw.split(",").map((entry) => entry.trim());
  for (const entry of entries) {
    if (!PIN_PATTERN.test(entry)) return null;
  }
  return entries;
}

/** Dedup an already-validated list of PINs against PINs seen so far. */
function dedup(entries: readonly string[], seen: Set<string>): string[] {
  const result: string[] = [];
  for (const entry of entries) {
    if (seen.has(entry)) continue;
    seen.add(entry);
    result.push(entry);
  }
  return result;
}

/** Parse the legacy three-line hands payload into a `QueueState`. */
export function parseHandsPayload(body: string): HandsOutcome {
  const lines = body.split("\n");
  if (lines.length < 3) {
    return { kind: "invalid", reason: "expected at least three lines" };
  }

  const [upcomingLine, currentLine, previousLine] = lines as [string, string, string];

  const upcomingEntries = parseLine(upcomingLine);
  if (upcomingEntries === null) {
    return {
      kind: "invalid",
      reason: "upcoming line is not NONE or a comma-separated list of 4-digit PINs"
    };
  }

  const currentEntries = parseLine(currentLine);
  if (currentEntries === null) {
    return {
      kind: "invalid",
      reason: "current line is not NONE or a comma-separated list of 4-digit PINs"
    };
  }
  if (currentEntries.length > 1) {
    return { kind: "invalid", reason: "current line must be NONE or a single 4-digit PIN" };
  }

  const previousEntries = parseLine(previousLine);
  if (previousEntries === null) {
    return {
      kind: "invalid",
      reason: "previous line is not NONE or a comma-separated list of 4-digit PINs"
    };
  }

  const seen = new Set<string>();

  let current: string | null = null;
  const [currentPin] = currentEntries;
  if (currentPin !== undefined) {
    current = currentPin;
    seen.add(currentPin);
  }

  const upcoming = dedup(upcomingEntries, seen);
  const previous = dedup(previousEntries, seen);

  return { kind: "data", queue: { previous, current, upcoming } };
}

/**
 * Remove the host and reader PINs from every list. A `null` chair PIN
 * removes nothing. If the current speaker is a chair, `current` becomes
 * `null` without promoting anyone from `upcoming` — promotion is an
 * operator decision, not automatic.
 */
export function stripChairs(
  queue: QueueState,
  chairs: { hostPin: string | null; readerPin: string | null }
): QueueState {
  const isChair = (pin: string): boolean =>
    pin === chairs.hostPin || pin === chairs.readerPin;

  return {
    previous: queue.previous.filter((pin) => !isChair(pin)),
    current: queue.current !== null && isChair(queue.current) ? null : queue.current,
    upcoming: queue.upcoming.filter((pin) => !isChair(pin))
  };
}

/** The display order: the current speaker first, then upcoming, in order. */
export function queueOrder(queue: QueueState): string[] {
  const order: string[] = [];
  if (queue.current !== null) order.push(queue.current);
  order.push(...queue.upcoming);
  return order;
}
