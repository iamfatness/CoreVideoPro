import { describe, expect, it } from "vitest";
import { FiloAssigner } from "./speakerRecency.js";

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
