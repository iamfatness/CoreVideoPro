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

/** Split a comma-separated list into trimmed, valid, deduplicated PINs. */
function parseList(raw: string, seen: Set<string>): string[] {
  const result: string[] = [];
  for (const rawEntry of raw.split(",")) {
    const entry = rawEntry.trim();
    if (entry.length === 0 || entry === "NONE") continue;
    if (!PIN_PATTERN.test(entry)) continue;
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

  const seen = new Set<string>();

  const currentEntry = currentLine.trim();
  let current: string | null = null;
  if (currentEntry.length > 0 && currentEntry !== "NONE" && PIN_PATTERN.test(currentEntry)) {
    current = currentEntry;
    seen.add(currentEntry);
  }

  const upcoming = parseList(upcomingLine, seen);
  const previous = parseList(previousLine, seen);

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
