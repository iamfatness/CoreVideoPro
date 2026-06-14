/**
 * Lower-third / nameplate manager.
 * Manages presenter identification plates (name, title, role) and a show queue:
 * one plate is on-air at a time, others wait in a queue. Pure TypeScript —
 * no React, no timers, no side effects.
 */

export type PlateTone = "neutral" | "accent" | "guest" | "breaking";

export type Nameplate = {
  id: string;
  name: string;
  title: string;
  /** Optional role/affiliation shown as a secondary tag. */
  role: string;
  tone: PlateTone;
};

export type PlateState = "queued" | "on-air" | "shown";

export type LowerThirdDeck = {
  id: string;
  plates: Nameplate[];
  /** Order of plate ids waiting to be shown. */
  queue: string[];
  /** The plate currently on-air, or null. */
  onAirId: string | null;
  /** Plate ids that have already aired. */
  shownIds: string[];
};

export type LowerThirdSummary = {
  total: number;
  queuedCount: number;
  shownCount: number;
  onAir: Nameplate | null;
  next: Nameplate | null;
  summary: string;
};

let _idCounter = 0;
function makeId(prefix = "plate"): string {
  _idCounter += 1;
  return `${prefix}-${Date.now()}-${_idCounter}`;
}

/**
 * Create an empty lower-third deck.
 */
export function createDeck(): LowerThirdDeck {
  return { id: makeId("deck"), plates: [], queue: [], onAirId: null, shownIds: [] };
}

/**
 * Add a nameplate to the deck and the back of the queue.
 * Trims text; throws if name is empty.
 */
export function addPlate(
  deck: LowerThirdDeck,
  name: string,
  title: string,
  role = "",
  tone: PlateTone = "neutral"
): LowerThirdDeck {
  const trimmedName = name.trim();
  if (trimmedName.length === 0) {
    throw new Error("Nameplate requires a name");
  }
  const plate: Nameplate = {
    id: makeId(),
    name: trimmedName,
    title: title.trim(),
    role: role.trim(),
    tone,
  };
  return {
    ...deck,
    plates: [...deck.plates, plate],
    queue: [...deck.queue, plate.id],
  };
}

/**
 * Remove a plate entirely from the deck (queue, on-air, shown).
 */
export function removePlate(deck: LowerThirdDeck, plateId: string): LowerThirdDeck {
  return {
    ...deck,
    plates: deck.plates.filter((p) => p.id !== plateId),
    queue: deck.queue.filter((id) => id !== plateId),
    onAirId: deck.onAirId === plateId ? null : deck.onAirId,
    shownIds: deck.shownIds.filter((id) => id !== plateId),
  };
}

/**
 * Look up a plate by id.
 */
export function getPlate(deck: LowerThirdDeck, plateId: string): Nameplate | null {
  return deck.plates.find((p) => p.id === plateId) ?? null;
}

/**
 * Determine the display state of a plate.
 */
export function plateState(deck: LowerThirdDeck, plateId: string): PlateState | null {
  if (deck.onAirId === plateId) return "on-air";
  if (deck.queue.includes(plateId)) return "queued";
  if (deck.shownIds.includes(plateId)) return "shown";
  return null;
}

/**
 * Put a specific plate on-air. Any currently on-air plate is taken down first
 * (moved to shown). The plate is removed from the queue. Unknown ids are ignored.
 */
export function showPlate(deck: LowerThirdDeck, plateId: string): LowerThirdDeck {
  if (!deck.plates.some((p) => p.id === plateId)) return deck;
  if (deck.onAirId === plateId) return deck;

  const shownIds = [...deck.shownIds];
  if (deck.onAirId !== null && !shownIds.includes(deck.onAirId)) {
    shownIds.push(deck.onAirId);
  }
  return {
    ...deck,
    onAirId: plateId,
    queue: deck.queue.filter((id) => id !== plateId),
    shownIds: shownIds.filter((id) => id !== plateId),
  };
}

/**
 * Take the on-air plate down (to shown). No-op if nothing is on-air.
 */
export function takeDown(deck: LowerThirdDeck): LowerThirdDeck {
  if (deck.onAirId === null) return deck;
  const shownIds = deck.shownIds.includes(deck.onAirId)
    ? deck.shownIds
    : [...deck.shownIds, deck.onAirId];
  return { ...deck, onAirId: null, shownIds };
}

/**
 * Show the next queued plate. No-op if the queue is empty.
 */
export function showNext(deck: LowerThirdDeck): LowerThirdDeck {
  const nextId = deck.queue[0];
  if (nextId === undefined) return deck;
  return showPlate(deck, nextId);
}

/**
 * Move a queued plate up or down within the queue.
 */
export function reorderQueue(deck: LowerThirdDeck, plateId: string, direction: "up" | "down"): LowerThirdDeck {
  const idx = deck.queue.indexOf(plateId);
  if (idx === -1) return deck;
  const swapWith = direction === "up" ? idx - 1 : idx + 1;
  if (swapWith < 0 || swapWith >= deck.queue.length) return deck;
  const queue = [...deck.queue];
  [queue[idx], queue[swapWith]] = [queue[swapWith], queue[idx]];
  return { ...deck, queue };
}

/**
 * Requeue a shown (or on-air) plate to the back of the queue.
 */
export function requeuePlate(deck: LowerThirdDeck, plateId: string): LowerThirdDeck {
  if (!deck.plates.some((p) => p.id === plateId)) return deck;
  if (deck.queue.includes(plateId)) return deck;
  return {
    ...deck,
    onAirId: deck.onAirId === plateId ? null : deck.onAirId,
    shownIds: deck.shownIds.filter((id) => id !== plateId),
    queue: [...deck.queue, plateId],
  };
}

/**
 * Ordered list of queued plates.
 */
export function queuedPlates(deck: LowerThirdDeck): Nameplate[] {
  return deck.queue
    .map((id) => deck.plates.find((p) => p.id === id))
    .filter((p): p is Nameplate => p !== undefined);
}

/**
 * Summarize deck state for display.
 */
export function summarizeDeck(deck: LowerThirdDeck): LowerThirdSummary {
  const onAir = deck.onAirId ? getPlate(deck, deck.onAirId) : null;
  const nextId = deck.queue[0];
  const next = nextId ? getPlate(deck, nextId) : null;

  const summary = onAir
    ? `On air: ${onAir.name}${onAir.title ? ` — ${onAir.title}` : ""} (${deck.queue.length} queued)`
    : deck.queue.length > 0
    ? `${deck.queue.length} plate${deck.queue.length !== 1 ? "s" : ""} queued, none on air`
    : "No plates queued";

  return {
    total: deck.plates.length,
    queuedCount: deck.queue.length,
    shownCount: deck.shownIds.length,
    onAir,
    next,
    summary,
  };
}

/**
 * Human-readable label for a plate tone.
 */
export function toneLabel(tone: PlateTone): string {
  switch (tone) {
    case "neutral": return "Standard";
    case "accent": return "Accent";
    case "guest": return "Guest";
    case "breaking": return "Breaking";
  }
}
