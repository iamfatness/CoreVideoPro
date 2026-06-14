import { describe, expect, it } from "vitest";
import {
  addPlate,
  createDeck,
  getPlate,
  plateState,
  queuedPlates,
  removePlate,
  reorderQueue,
  requeuePlate,
  showNext,
  showPlate,
  summarizeDeck,
  takeDown,
  toneLabel,
} from "./lowerThird";

function deckWith(...names: string[]) {
  let d = createDeck();
  for (const n of names) {
    d = addPlate(d, n, `${n}'s title`);
  }
  return d;
}

describe("createDeck", () => {
  it("creates an empty deck", () => {
    const d = createDeck();
    expect(d.plates).toHaveLength(0);
    expect(d.queue).toHaveLength(0);
    expect(d.onAirId).toBeNull();
    expect(d.shownIds).toHaveLength(0);
  });
});

describe("addPlate", () => {
  it("adds a plate to deck and queue", () => {
    const d = addPlate(createDeck(), "Ada Lovelace", "Mathematician");
    expect(d.plates).toHaveLength(1);
    expect(d.queue).toHaveLength(1);
    expect(d.queue[0]).toBe(d.plates[0].id);
  });

  it("trims name, title, role", () => {
    const d = addPlate(createDeck(), "  Ada  ", "  Math  ", "  Analyst  ");
    expect(d.plates[0].name).toBe("Ada");
    expect(d.plates[0].title).toBe("Math");
    expect(d.plates[0].role).toBe("Analyst");
  });

  it("defaults tone to neutral", () => {
    expect(addPlate(createDeck(), "Ada", "Math").plates[0].tone).toBe("neutral");
  });

  it("accepts a tone", () => {
    expect(addPlate(createDeck(), "Ada", "Math", "", "guest").plates[0].tone).toBe("guest");
  });

  it("throws on empty name", () => {
    expect(() => addPlate(createDeck(), "   ", "Math")).toThrow();
  });
});

describe("removePlate", () => {
  it("removes from deck and queue", () => {
    const d = deckWith("A", "B");
    const id = d.plates[0].id;
    const updated = removePlate(d, id);
    expect(updated.plates).toHaveLength(1);
    expect(updated.queue).not.toContain(id);
  });

  it("clears on-air if the removed plate was on air", () => {
    let d = deckWith("A");
    const id = d.plates[0].id;
    d = showPlate(d, id);
    d = removePlate(d, id);
    expect(d.onAirId).toBeNull();
  });
});

describe("getPlate / plateState", () => {
  it("looks up a plate", () => {
    const d = deckWith("A");
    expect(getPlate(d, d.plates[0].id)?.name).toBe("A");
  });

  it("returns null for unknown plate", () => {
    expect(getPlate(deckWith("A"), "nope")).toBeNull();
  });

  it("reports queued state", () => {
    const d = deckWith("A");
    expect(plateState(d, d.plates[0].id)).toBe("queued");
  });

  it("reports on-air state", () => {
    let d = deckWith("A");
    d = showPlate(d, d.plates[0].id);
    expect(plateState(d, d.plates[0].id)).toBe("on-air");
  });

  it("reports shown state after take-down", () => {
    let d = deckWith("A");
    d = takeDown(showPlate(d, d.plates[0].id));
    expect(plateState(d, d.plates[0].id)).toBe("shown");
  });

  it("returns null for an unknown plate state", () => {
    expect(plateState(deckWith("A"), "nope")).toBeNull();
  });
});

describe("showPlate", () => {
  it("puts a plate on air and removes it from the queue", () => {
    let d = deckWith("A");
    const id = d.plates[0].id;
    d = showPlate(d, id);
    expect(d.onAirId).toBe(id);
    expect(d.queue).not.toContain(id);
  });

  it("takes the previous plate down to shown when showing another", () => {
    let d = deckWith("A", "B");
    const a = d.plates[0].id;
    const b = d.plates[1].id;
    d = showPlate(d, a);
    d = showPlate(d, b);
    expect(d.onAirId).toBe(b);
    expect(d.shownIds).toContain(a);
  });

  it("ignores unknown plate ids", () => {
    const d = deckWith("A");
    expect(showPlate(d, "nope").onAirId).toBeNull();
  });

  it("is a no-op if the plate is already on air", () => {
    let d = deckWith("A");
    const id = d.plates[0].id;
    d = showPlate(d, id);
    const again = showPlate(d, id);
    expect(again).toEqual(d);
  });
});

describe("takeDown", () => {
  it("moves on-air plate to shown", () => {
    let d = deckWith("A");
    const id = d.plates[0].id;
    d = takeDown(showPlate(d, id));
    expect(d.onAirId).toBeNull();
    expect(d.shownIds).toContain(id);
  });

  it("is a no-op when nothing is on air", () => {
    const d = deckWith("A");
    expect(takeDown(d)).toEqual(d);
  });
});

describe("showNext", () => {
  it("shows the front of the queue", () => {
    let d = deckWith("A", "B");
    const a = d.plates[0].id;
    d = showNext(d);
    expect(d.onAirId).toBe(a);
  });

  it("advances through the queue", () => {
    let d = deckWith("A", "B");
    const b = d.plates[1].id;
    d = showNext(d); // A on air
    d = showNext(d); // B on air, A shown
    expect(d.onAirId).toBe(b);
  });

  it("is a no-op on empty queue", () => {
    const d = createDeck();
    expect(showNext(d)).toEqual(d);
  });
});

describe("reorderQueue", () => {
  it("moves a plate up", () => {
    let d = deckWith("A", "B", "C");
    const b = d.plates[1].id;
    d = reorderQueue(d, b, "up");
    expect(d.queue[0]).toBe(b);
  });

  it("moves a plate down", () => {
    let d = deckWith("A", "B", "C");
    const a = d.plates[0].id;
    d = reorderQueue(d, a, "down");
    expect(d.queue[1]).toBe(a);
  });

  it("does not move the first plate up", () => {
    const d = deckWith("A", "B");
    const a = d.plates[0].id;
    expect(reorderQueue(d, a, "up").queue).toEqual(d.queue);
  });

  it("does not move the last plate down", () => {
    const d = deckWith("A", "B");
    const b = d.plates[1].id;
    expect(reorderQueue(d, b, "down").queue).toEqual(d.queue);
  });

  it("ignores plates not in the queue", () => {
    const d = deckWith("A");
    expect(reorderQueue(d, "nope", "up").queue).toEqual(d.queue);
  });
});

describe("requeuePlate", () => {
  it("returns a shown plate to the back of the queue", () => {
    let d = deckWith("A", "B");
    const a = d.plates[0].id;
    d = takeDown(showPlate(d, a)); // A shown
    d = requeuePlate(d, a);
    expect(d.queue[d.queue.length - 1]).toBe(a);
    expect(d.shownIds).not.toContain(a);
  });

  it("clears on-air when requeueing the on-air plate", () => {
    let d = deckWith("A");
    const a = d.plates[0].id;
    d = showPlate(d, a);
    d = requeuePlate(d, a);
    expect(d.onAirId).toBeNull();
    expect(d.queue).toContain(a);
  });

  it("is a no-op for a plate already queued", () => {
    const d = deckWith("A");
    expect(requeuePlate(d, d.plates[0].id)).toEqual(d);
  });
});

describe("queuedPlates", () => {
  it("returns plates in queue order", () => {
    const d = deckWith("A", "B", "C");
    expect(queuedPlates(d).map((p) => p.name)).toEqual(["A", "B", "C"]);
  });

  it("reflects reordering", () => {
    let d = deckWith("A", "B");
    d = reorderQueue(d, d.plates[1].id, "up");
    expect(queuedPlates(d).map((p) => p.name)).toEqual(["B", "A"]);
  });
});

describe("summarizeDeck", () => {
  it("counts totals", () => {
    let d = deckWith("A", "B", "C");
    d = takeDown(showPlate(d, d.plates[0].id)); // A shown
    const s = summarizeDeck(d);
    expect(s.total).toBe(3);
    expect(s.queuedCount).toBe(2);
    expect(s.shownCount).toBe(1);
  });

  it("exposes the on-air plate", () => {
    let d = deckWith("A");
    d = showPlate(d, d.plates[0].id);
    expect(summarizeDeck(d).onAir?.name).toBe("A");
    expect(summarizeDeck(d).summary).toContain("On air:");
  });

  it("exposes the next plate", () => {
    const d = deckWith("A", "B");
    expect(summarizeDeck(d).next?.name).toBe("A");
  });

  it("summary handles empty deck", () => {
    expect(summarizeDeck(createDeck()).summary).toBe("No plates queued");
  });

  it("summary reflects queued count when nothing is on air", () => {
    expect(summarizeDeck(deckWith("A", "B")).summary).toContain("2 plates queued");
  });
});

describe("toneLabel", () => {
  it("neutral → Standard", () => expect(toneLabel("neutral")).toBe("Standard"));
  it("accent → Accent", () => expect(toneLabel("accent")).toBe("Accent"));
  it("guest → Guest", () => expect(toneLabel("guest")).toBe("Guest"));
  it("breaking → Breaking", () => expect(toneLabel("breaking")).toBe("Breaking"));
});
