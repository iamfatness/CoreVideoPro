import { describe, expect, it } from "vitest";
import { FiloAssigner, RecencyScores, VisibleSetAssigner } from "./speakerRecency.js";

describe("FiloAssigner", () => {
  it("rejects a capacity below one", () => {
    expect(() => new FiloAssigner({ capacity: 0 })).toThrow(/capacity/);
  });

  it("starts empty", () => {
    expect(new FiloAssigner({ capacity: 3 }).positions().size).toBe(0);
  });

  it("fills free positions in ascending order", () => {
    const filo = new FiloAssigner({ capacity: 3 });
    expect(filo.onActiveSpeaker("a")).toEqual([{ position: 1, participantId: "a" }]);
    expect(filo.onActiveSpeaker("b")).toEqual([{ position: 2, participantId: "b" }]);
    expect([...filo.positions().entries()]).toEqual([
      [1, "a"],
      [2, "b"]
    ]);
  });

  it("returns no changes when an already-placed speaker speaks again", () => {
    const filo = new FiloAssigner({ capacity: 3 });
    filo.onActiveSpeaker("a");
    expect(filo.onActiveSpeaker("a")).toEqual([]);
    expect(filo.positions().get(1)).toBe("a");
  });

  it("evicts the least recently active occupant when full", () => {
    const filo = new FiloAssigner({ capacity: 2 });
    filo.onActiveSpeaker("a");
    filo.onActiveSpeaker("b");
    expect(filo.onActiveSpeaker("c")).toEqual([{ position: 1, participantId: "c" }]);
    expect([...filo.positions().entries()]).toEqual([
      [1, "c"],
      [2, "b"]
    ]);
  });

  it("protects a speaker who refreshed their recency from the next eviction", () => {
    const filo = new FiloAssigner({ capacity: 2 });
    filo.onActiveSpeaker("a");
    filo.onActiveSpeaker("b");
    filo.onActiveSpeaker("a");
    expect(filo.onActiveSpeaker("c")).toEqual([{ position: 2, participantId: "c" }]);
    expect([...filo.positions().entries()]).toEqual([
      [1, "a"],
      [2, "c"]
    ]);
  });

  it("never evicts the current speaker", () => {
    const filo = new FiloAssigner({ capacity: 1 });
    filo.onActiveSpeaker("a");
    filo.onActiveSpeaker("b");
    expect(filo.positions().get(1)).toBe("b");
    expect(filo.onActiveSpeaker("b")).toEqual([]);
    expect(filo.positions().get(1)).toBe("b");
  });

  it("seats a roster in order on reset, treating the last as most recent", () => {
    const filo = new FiloAssigner({ capacity: 3 });
    filo.reset(["a", "b", "c"]);
    expect([...filo.positions().entries()]).toEqual([
      [1, "a"],
      [2, "b"],
      [3, "c"]
    ]);
    expect(filo.onActiveSpeaker("d")).toEqual([{ position: 1, participantId: "d" }]);
  });

  it("drops entries past capacity on reset", () => {
    const filo = new FiloAssigner({ capacity: 2 });
    filo.reset(["a", "b", "c"]);
    expect([...filo.positions().entries()]).toEqual([
      [1, "a"],
      [2, "b"]
    ]);
  });

  it("empties the pool on an empty reset", () => {
    const filo = new FiloAssigner({ capacity: 2 });
    filo.onActiveSpeaker("a");
    filo.reset([]);
    expect(filo.positions().size).toBe(0);
  });

  it("returns a fresh map so callers cannot mutate internal state", () => {
    const filo = new FiloAssigner({ capacity: 2 });
    filo.onActiveSpeaker("a");
    filo.positions().set(1, "hacked");
    expect(filo.positions().get(1)).toBe("a");
  });
});

describe("VisibleSetAssigner", () => {
  it("rejects invalid geometry", () => {
    expect(() => new VisibleSetAssigner({ capacity: 4, visible: 0 })).toThrow(/visible/);
    expect(() => new VisibleSetAssigner({ capacity: 4, visible: 5 })).toThrow(/visible/);
    expect(() => new VisibleSetAssigner({ capacity: 0, visible: 1 })).toThrow(/capacity/);
  });

  it("seats newcomers into visible positions first", () => {
    const set = new VisibleSetAssigner({ capacity: 4, visible: 2 });
    expect(set.onActiveSpeaker("a")).toEqual([{ position: 1, participantId: "a" }]);
    expect(set.onActiveSpeaker("b")).toEqual([{ position: 2, participantId: "b" }]);
  });

  it("returns no changes when a visible speaker speaks again", () => {
    const set = new VisibleSetAssigner({ capacity: 4, visible: 2 });
    set.onActiveSpeaker("a");
    expect(set.onActiveSpeaker("a")).toEqual([]);
  });

  it("swaps an off-screen speaker into the stalest visible position", () => {
    const set = new VisibleSetAssigner({ capacity: 4, visible: 2 });
    set.reset(["a", "b", "c", "d"]);
    set.onActiveSpeaker("b");

    const changes = set.onActiveSpeaker("d");
    expect(changes).toHaveLength(2);
    expect(set.positions().get(1)).toBe("d");
    expect(set.positions().get(4)).toBe("a");
    expect(set.positions().get(2)).toBe("b");
  });

  it("keeps everyone seated — a swap never drops a participant", () => {
    const set = new VisibleSetAssigner({ capacity: 3, visible: 1 });
    set.reset(["a", "b", "c"]);
    set.onActiveSpeaker("c");
    expect([...set.positions().values()].sort()).toEqual(["a", "b", "c"]);
  });

  it("breaks recency ties by lower position number", () => {
    const set = new VisibleSetAssigner({ capacity: 4, visible: 2 });
    set.reset(["a", "b", "c", "d"]);
    set.onActiveSpeaker("d");
    expect(set.positions().get(1)).toBe("d");
  });

  it("returns a fresh map so callers cannot mutate internal state", () => {
    const set = new VisibleSetAssigner({ capacity: 2, visible: 1 });
    set.onActiveSpeaker("a");
    set.positions().set(1, "hacked");
    expect(set.positions().get(1)).toBe("a");
  });

  it("evicts the least recently active occupant overall when an unknown speaker arrives at a full pool, then applies the standard swap if their freed seat isn't visible", () => {
    // capacity 3, visible 1: only position 1 is on screen.
    const set = new VisibleSetAssigner({ capacity: 3, visible: 1 });
    set.reset(["a", "b", "c"]);
    // Refresh "a" so it is no longer the least recently active overall;
    // "b" (sitting off-screen at position 2) becomes the stalest occupant.
    set.onActiveSpeaker("a");

    const changes = set.onActiveSpeaker("d");

    // "b" was the least recently active occupant overall (visible or not),
    // so it is evicted entirely to free position 2 for "d". Position 2 is
    // not visible, so the standard not-visible rule then swaps "d" into the
    // stalest visible position (1, held by "a"), and "a" takes the freed
    // off-screen seat instead of "b".
    expect(changes).toHaveLength(2);
    expect(set.positions().get(1)).toBe("d");
    expect(set.positions().get(2)).toBe("a");
    expect(set.positions().get(3)).toBe("c");
    expect(set.positions().size).toBe(3);
    expect([...set.positions().values()]).not.toContain("b");
  });
});

describe("RecencyScores", () => {
  it("returns an unranked roster in its given order", () => {
    const scores = new RecencyScores();
    expect(scores.order(["a", "b", "c"])).toEqual(["a", "b", "c"]);
  });

  it("puts the most recent speaker first", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("c");
    expect(scores.order(["a", "b", "c"])).toEqual(["c", "a", "b"]);
  });

  it("orders several speakers most-recent-first", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("a");
    scores.onActiveSpeaker("b");
    scores.onActiveSpeaker("c");
    expect(scores.order(["a", "b", "c"])).toEqual(["c", "b", "a"]);
  });

  it("re-promotes a repeat speaker", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("a");
    scores.onActiveSpeaker("b");
    scores.onActiveSpeaker("a");
    expect(scores.order(["a", "b"])).toEqual(["a", "b"]);
  });

  it("sorts never-spoken participants last, stably", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("c");
    expect(scores.order(["a", "b", "c", "d"])).toEqual(["c", "a", "b", "d"]);
  });

  it("returns a permutation of the input, ignoring unknown history", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("z");
    expect(scores.order(["a", "b"]).sort()).toEqual(["a", "b"]);
  });

  it("clears history on reset", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("c");
    scores.reset();
    expect(scores.order(["a", "b", "c"])).toEqual(["a", "b", "c"]);
  });

  it("returns a fresh array each call", () => {
    const scores = new RecencyScores();
    const first = scores.order(["a", "b"]);
    first.push("nope");
    expect(scores.order(["a", "b"])).toEqual(["a", "b"]);
  });
});
