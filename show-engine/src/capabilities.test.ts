import { describe, expect, it } from "vitest";
import { canUse, resolveCapabilities, type HealthByEndpoint } from "./capabilities.js";
import { parseShowEngineConfig } from "./config.js";

/**
 * A show with nothing configured — and therefore no Mukana block at all.
 * Supplying one here would quietly assert the opposite of what this suite
 * is about: an un-integrated show must not have to name an address the
 * engine will never call.
 */
const base = {
  capacity: 8,
  statePath: "/show/state.json"
};

function health(overrides: Partial<HealthByEndpoint> = {}): HealthByEndpoint {
  const ok = { state: "ok" as const, consecutiveFailures: 0, detail: null };
  return { panelists: ok, hands: ok, question: ok, ...overrides };
}

const allOn = parseShowEngineConfig({
  ...base,
  mukana: { baseUrl: "https://example.com/rest.php", event: "officehours" },
  integrations: { registry: true, handsQueue: true, questionFeed: true }
});

describe("resolveCapabilities", () => {
  it("reports every capability disabled when nothing is configured", () => {
    const config = parseShowEngineConfig(base);
    expect(config.mukana).toBeNull();
    const caps = resolveCapabilities(config, health());
    expect(caps.registry).toEqual({ state: "disabled", detail: null });
    expect(caps.handsQueue).toEqual({ state: "disabled", detail: null });
    expect(caps.questionFeed).toEqual({ state: "disabled", detail: null });
  });

  it("reports every capability disabled with no Mukana even when its endpoints are failing", () => {
    const failing = { state: "failing" as const, consecutiveFailures: 9, detail: "HTTP 503" };
    const caps = resolveCapabilities(parseShowEngineConfig(base), {
      panelists: failing,
      hands: failing,
      question: failing
    });
    expect(caps.registry).toEqual({ state: "disabled", detail: null });
    expect(caps.handsQueue).toEqual({ state: "disabled", detail: null });
    expect(caps.questionFeed).toEqual({ state: "disabled", detail: null });
  });

  it("reports a configured, healthy integration as available", () => {
    const caps = resolveCapabilities(allOn, health());
    expect(caps.registry).toEqual({ state: "available", detail: null });
  });

  it("maps each capability to its own endpoint", () => {
    const caps = resolveCapabilities(
      allOn,
      health({ hands: { state: "failing", consecutiveFailures: 2, detail: "HTTP 503" } })
    );
    expect(caps.handsQueue.state).toBe("unavailable");
    expect(caps.registry.state).toBe("available");
    expect(caps.questionFeed.state).toBe("available");
  });

  it("carries the health detail on an unavailable capability", () => {
    const caps = resolveCapabilities(
      allOn,
      health({ hands: { state: "failing", consecutiveFailures: 1, detail: "HTTP 503" } })
    );
    expect(caps.handsQueue.detail).toBe("HTTP 503");
  });

  it("treats dormant as unavailable and keeps its detail", () => {
    const caps = resolveCapabilities(
      allOn,
      health({
        panelists: { state: "dormant", consecutiveFailures: 0, detail: "outside show hours" }
      })
    );
    expect(caps.registry).toEqual({ state: "unavailable", detail: "outside show hours" });
  });

  it("supplies a detail when the health record has none", () => {
    const caps = resolveCapabilities(
      allOn,
      health({ hands: { state: "failing", consecutiveFailures: 1, detail: null } })
    );
    expect(caps.handsQueue.state).toBe("unavailable");
    expect(caps.handsQueue.detail).not.toBeNull();
    expect(caps.handsQueue.detail).not.toBe("");
  });

  it("reports disabled regardless of health when not configured", () => {
    const caps = resolveCapabilities(
      parseShowEngineConfig(base),
      health({ hands: { state: "failing", consecutiveFailures: 9, detail: "HTTP 503" } })
    );
    expect(caps.handsQueue).toEqual({ state: "disabled", detail: null });
  });

  it("returns a fresh object each call", () => {
    const first = resolveCapabilities(allOn, health());
    first.registry.state = "disabled";
    expect(resolveCapabilities(allOn, health()).registry.state).toBe("available");
  });
});

describe("canUse", () => {
  it("is true only for available", () => {
    expect(canUse({ state: "available", detail: null })).toBe(true);
    expect(canUse({ state: "unavailable", detail: "HTTP 503" })).toBe(false);
    expect(canUse({ state: "disabled", detail: null })).toBe(false);
  });
});
