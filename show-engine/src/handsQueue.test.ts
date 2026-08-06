import { describe, expect, it } from "vitest";
import { parseHandsPayload, queueOrder, stripChairs } from "./handsQueue.js";
import type { QueueState } from "./contracts.js";

describe("parseHandsPayload", () => {
  it("parses upcoming, current and previous", () => {
    const outcome = parseHandsPayload("4242,5555\n1383\n9999,8888");
    expect(outcome).toEqual({
      kind: "data",
      queue: { previous: ["9999", "8888"], current: "1383", upcoming: ["4242", "5555"] }
    });
  });

  it("treats NONE as an empty list", () => {
    const outcome = parseHandsPayload("NONE\n1383\nNONE");
    expect(outcome).toEqual({
      kind: "data",
      queue: { previous: [], current: "1383", upcoming: [] }
    });
  });

  it("treats a NONE current as null", () => {
    const outcome = parseHandsPayload("4242\nNONE\nNONE");
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.queue.current).toBeNull();
  });

  it("drops entries that are not four digits", () => {
    const outcome = parseHandsPayload("4242,abcd,12345,777\nNONE\nNONE");
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.queue.upcoming).toEqual(["4242"]);
  });

  it("removes duplicates, keeping the first occurrence", () => {
    const outcome = parseHandsPayload("1383,4242\n1383\n4242");
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.queue).toEqual({ previous: [], current: "1383", upcoming: ["4242"] });
  });

  it("trims whitespace around entries", () => {
    const outcome = parseHandsPayload(" 4242 , 5555 \n 1383 \nNONE");
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.queue.upcoming).toEqual(["4242", "5555"]);
    expect(outcome.queue.current).toBe("1383");
  });

  it("rejects a body with fewer than three lines", () => {
    expect(parseHandsPayload("4242\n1383").kind).toBe("invalid");
    expect(parseHandsPayload("").kind).toBe("invalid");
  });
});

describe("stripChairs", () => {
  const queue: QueueState = {
    previous: ["9999", "1383"],
    current: "4242",
    upcoming: ["1383", "5555", "7777"]
  };

  it("removes the host and reader from every list", () => {
    expect(stripChairs(queue, { hostPin: "1383", readerPin: "5555" })).toEqual({
      previous: ["9999"],
      current: "4242",
      upcoming: ["7777"]
    });
  });

  it("nulls the current entry when it is a chair", () => {
    expect(stripChairs(queue, { hostPin: "4242", readerPin: null })).toEqual({
      previous: ["9999", "1383"],
      current: null,
      upcoming: ["1383", "5555", "7777"]
    });
  });

  it("does not promote from upcoming when the current entry is removed", () => {
    const result = stripChairs(queue, { hostPin: "4242", readerPin: null });
    expect(result.current).toBeNull();
    expect(result.upcoming[0]).toBe("1383");
  });

  it("removes nothing when both chairs are null", () => {
    expect(stripChairs(queue, { hostPin: null, readerPin: null })).toEqual(queue);
  });

  it("does not mutate its input", () => {
    const input: QueueState = { previous: ["1383"], current: "1383", upcoming: ["1383"] };
    stripChairs(input, { hostPin: "1383", readerPin: null });
    expect(input).toEqual({ previous: ["1383"], current: "1383", upcoming: ["1383"] });
  });
});

describe("queueOrder", () => {
  it("puts the current entry first, then upcoming", () => {
    expect(
      queueOrder({ previous: ["9999"], current: "1383", upcoming: ["4242", "5555"] })
    ).toEqual(["1383", "4242", "5555"]);
  });

  it("omits a null current", () => {
    expect(queueOrder({ previous: [], current: null, upcoming: ["4242"] })).toEqual(["4242"]);
  });

  it("excludes previously shown entries", () => {
    expect(queueOrder({ previous: ["9999"], current: null, upcoming: [] })).toEqual([]);
  });

  it("returns a fresh array each call", () => {
    const queue: QueueState = { previous: [], current: "1383", upcoming: [] };
    const first = queueOrder(queue);
    first.push("nope");
    expect(queueOrder(queue)).toEqual(["1383"]);
  });
});
