/**
 * Direct tests for the conformance suite's own comparison helper.
 *
 * `HOST_CONFORMANCE_CASES` itself is executed in `actionsPipeline.test.ts`
 * against three host shapes (and again from `dist` by
 * `scripts/verify-dist-barrel.mjs`). This file covers the one thing those
 * runs structurally cannot: `canonicalize`'s behavior on values no SHIPPED
 * case happens to compare. Every value a current case asserts on is built
 * from distinct objects, so a suite-level test of the diamond property
 * below would pass with the bug in place — which is precisely how the bug
 * shipped past a round of review that ran the whole suite three ways.
 *
 * `canonicalize` is imported directly from `./conformance.js` rather than
 * through `./index.js`: it is an internal helper of the suite, exported for
 * this test and deliberately absent from the package barrel (which names
 * its exports one by one), because a host adapter has no reason to call it.
 */

import { describe, expect, it } from "vitest";
import { canonicalize } from "./conformance.js";

/** What `assertEqual` actually compares: the canonical form, serialized. */
function describeValue(value: unknown): string {
  return JSON.stringify(canonicalize(value)) ?? String(value);
}

describe("canonicalize — shared sub-objects are not cycles", () => {
  /**
   * Final fix round, I2. `seen` used to be threaded through the WHOLE
   * recursion and never released, so it detected repeated REFERENCES rather
   * than cycles: the diamond below serialized its second branch as the
   * string `"[circular]"` while the same structure built from two separate
   * literals serialized in full, making two equal values compare unequal.
   *
   * The victim is the consumer `ConformanceHost` was widened for: a Plan
   * 7-9 recording facade that interns or memoises any repeated value got a
   * failure message saying `[circular]` about a structure with no cycle.
   */
  it("serializes a value reached twice by different paths in full, both times", () => {
    const shared = { a: 1 };
    const diamond = { left: shared, right: shared };

    expect(describeValue(diamond)).toBe('{"left":{"a":1},"right":{"a":1}}');
  });

  /** The comparison that failed: interned vs. freshly-built must be equal. */
  it("compares an interned structure equal to the same structure built from literals", () => {
    const emptyBox: [number, null] = [2, null];
    const interned = { boxes: [emptyBox, emptyBox] };
    const literals = { boxes: [[2, null], [2, null]] };

    expect(describeValue(interned)).toBe(describeValue(literals));
  });

  /** Siblings, not just parent/child: three references to one value all render. */
  it("renders every sibling reference to one shared value", () => {
    const cell = { slot: 0 };
    expect(describeValue([cell, cell, cell])).toBe('[{"slot":0},{"slot":0},{"slot":0}]');
  });

  /**
   * The property the released `ancestors` set must NOT give up: a genuine
   * cycle still terminates with `[circular]` rather than recursing until
   * the stack blows. Mutation target: delete the `ancestors.has` guard and
   * this test dies with a RangeError instead of an assertion failure.
   */
  it("still reports a genuine cycle rather than recursing forever", () => {
    const cyclic: Record<string, unknown> = { name: "loop" };
    cyclic.self = cyclic;

    expect(describeValue(cyclic)).toBe('{"name":"loop","self":"[circular]"}');
  });

  /** A cycle through an array, and one through a `Map`, take the same path. */
  it("reports a cycle reached through an array or a Map", () => {
    const arr: unknown[] = [1];
    arr.push(arr);
    expect(describeValue(arr)).toBe('[1,"[circular]"]');

    const map = new Map<string, unknown>();
    map.set("self", map);
    expect(describeValue(map)).toBe('[["self","[circular]"]]');
  });
});

describe("canonicalize — the properties the suite already depended on", () => {
  /** The fix-round-1 property: object KEY order must not affect equality. */
  it("normalizes object key order at every depth", () => {
    const written = { kind: "applyLook", boxes: [{ box: 1, slot: 3 }] };
    const reordered = { boxes: [{ slot: 3, box: 1 }], kind: "applyLook" };

    expect(describeValue(written)).toBe(describeValue(reordered));
  });

  /** And ARRAY order must still be load-bearing — emission order is a real assertion. */
  it("preserves array order", () => {
    expect(describeValue([1, 2, 3])).not.toBe(describeValue([3, 2, 1]));
  });

  it("leaves scalars and null untouched", () => {
    expect(canonicalize(null)).toBeNull();
    expect(canonicalize(7)).toBe(7);
    expect(canonicalize("x")).toBe("x");
    expect(canonicalize(true)).toBe(true);
  });
});
