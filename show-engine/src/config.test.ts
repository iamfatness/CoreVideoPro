import { describe, expect, it } from "vitest";
import { parseShowEngineConfig } from "./config.js";

const minimal = {
  capacity: 16,
  statePath: "/var/tmp/ohg-state.json",
  mukana: { baseUrl: "https://hoka.example.com/php-panel-rest.php", event: "officehours" }
};

describe("parseShowEngineConfig", () => {
  it("applies defaults for omitted optional fields", () => {
    const config = parseShowEngineConfig(minimal);
    expect(config.capacity).toBe(16);
    expect(config.utilityPinBase).toBe(9000);
    expect(config.mukana.panelistsIntervalMs).toBe(5000);
    expect(config.mukana.handsIntervalMs).toBe(2000);
    expect(config.mukana.maxBackoffMs).toBe(60000);
  });

  it("keeps explicitly provided values", () => {
    const config = parseShowEngineConfig({
      ...minimal,
      utilityPinBase: 8000,
      mukana: { ...minimal.mukana, panelistsIntervalMs: 1500 }
    });
    expect(config.utilityPinBase).toBe(8000);
    expect(config.mukana.panelistsIntervalMs).toBe(1500);
  });

  it("rejects a capacity below 1", () => {
    expect(() => parseShowEngineConfig({ ...minimal, capacity: 0 })).toThrow(/capacity/);
  });

  it("rejects a missing mukana baseUrl", () => {
    expect(() => parseShowEngineConfig({ ...minimal, mukana: { event: "officehours" } })).toThrow(
      /mukana\.baseUrl/
    );
  });

  it("rejects a non-object config", () => {
    expect(() => parseShowEngineConfig("nope")).toThrow(/config/);
  });
});
