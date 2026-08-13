/**
 * Barrel completeness test for the capability layer. Asserts that every
 * runtime value and type the capability layer needs is actually reachable
 * from the package's public entry point, not merely defined somewhere
 * inside it. A type-only export cannot be asserted at runtime, so those
 * names are imported in a type position and carried by the typecheck: a
 * file that fails to typecheck is a failing test.
 */

import { describe, expect, it } from "vitest";
import {
  BOX_FILLS,
  canUse,
  effectiveBoxFill,
  formatProgramSource,
  invokeAction,
  isBoxFill,
  oscAddressFor,
  OHG_ACTIONS,
  parseProgramSource,
  personKeyForPin,
  resolveCapabilities,
  resolvePersonKey,
  type ActionDefinition,
  type ActionParam,
  type ActionParamType,
  type ActionResult,
  type Capability,
  type CapabilityState,
  type HealthByEndpoint,
  type ManualBoxAssignments,
  type PersonKey,
  type ShowCapabilities,
  type BoxFill
} from "./index.js";

describe("capability layer exports", () => {
  it("exports every runtime value the capability layer needs", () => {
    expect(resolvePersonKey).toBeTypeOf("function");
    expect(personKeyForPin).toBeTypeOf("function");
    expect(resolveCapabilities).toBeTypeOf("function");
    expect(canUse).toBeTypeOf("function");
    expect(effectiveBoxFill).toBeTypeOf("function");
    expect(isBoxFill).toBeTypeOf("function");
    expect(BOX_FILLS).toEqual(["queue", "manual"]);
  });

  it("exports every type a host adapter needs to name", () => {
    const key: PersonKey = "pin:1383";
    const state: CapabilityState = "available";
    const capability: Capability = { state, detail: null };
    const caps: ShowCapabilities = {
      registry: capability,
      handsQueue: capability,
      questionFeed: capability
    };
    const fill: BoxFill = "manual";
    const manual: ManualBoxAssignments = { 1: 3 };
    const health: HealthByEndpoint = {
      panelists: { state: "ok", consecutiveFailures: 0, detail: null },
      hands: { state: "ok", consecutiveFailures: 0, detail: null },
      question: { state: "ok", consecutiveFailures: 0, detail: null }
    };
    expect([key, caps, fill, manual, health]).toHaveLength(5);
  });
});

describe("ohg action registry exports", () => {
  it("exports every runtime value a host bridge needs", () => {
    expect(invokeAction).toBeTypeOf("function");
    expect(oscAddressFor).toBeTypeOf("function");
    expect(parseProgramSource).toBeTypeOf("function");
    expect(formatProgramSource).toBeTypeOf("function");
    expect(OHG_ACTIONS.length).toBeGreaterThan(0);
  });

  it("exports every type a host bridge needs to name", () => {
    const paramType: ActionParamType = "string";
    const param: ActionParam = { name: "source", type: paramType, required: true, description: "" };
    const definition: ActionDefinition = {
      id: "ohg.program.preview",
      title: "Stage preview",
      description: "",
      params: [param]
    };
    const result: ActionResult = { kind: "ok" };
    expect([param, definition, result]).toHaveLength(3);
  });
});
