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

const look = {
  id: "hr",
  label: "Host + Reader",
  scenePreset: "scene-hr",
  boxes: 2,
  includesHost: true,
  includesReader: true,
  plateTone: "accent",
  tallySource: "boxes"
};

describe("parseShowEngineConfig direction fields", () => {
  it("defaults skipRoles to the ASL interpreter and looks to empty", () => {
    const config = parseShowEngineConfig(minimal);
    expect(config.skipRoles).toEqual(["aslinterpreter"]);
    expect(config.looks).toEqual([]);
  });

  it("keeps explicitly provided skipRoles", () => {
    const config = parseShowEngineConfig({ ...minimal, skipRoles: ["aslinterpreter", "reader"] });
    expect(config.skipRoles).toEqual(["aslinterpreter", "reader"]);
  });

  it("rejects an unknown role rather than coercing it", () => {
    expect(() => parseShowEngineConfig({ ...minimal, skipRoles: ["moderator"] })).toThrow(
      /skipRoles/
    );
  });

  it("accepts a well-formed look", () => {
    const config = parseShowEngineConfig({ ...minimal, looks: [look] });
    expect(config.looks).toEqual([look]);
  });

  it("rejects a look with an out-of-range box count", () => {
    expect(() => parseShowEngineConfig({ ...minimal, looks: [{ ...look, boxes: 5 }] })).toThrow(
      /boxes/
    );
  });

  it("rejects a look with an unknown plateTone", () => {
    expect(() =>
      parseShowEngineConfig({ ...minimal, looks: [{ ...look, plateTone: "chartreuse" }] })
    ).toThrow(/plateTone/);
  });

  it("defaults an omitted plateTone to neutral", () => {
    const { plateTone: _omitted, ...withoutTone } = look;
    const config = parseShowEngineConfig({ ...minimal, looks: [withoutTone] });
    expect(config.looks[0]?.plateTone).toBe("neutral");
  });

  it("rejects a look with an unknown tallySource", () => {
    expect(() =>
      parseShowEngineConfig({ ...minimal, looks: [{ ...look, tallySource: "everyone" }] })
    ).toThrow(/tallySource/);
  });

  it("defaults an omitted tallySource to boxes", () => {
    const { tallySource: _omitted, ...withoutTallySource } = look;
    const config = parseShowEngineConfig({ ...minimal, looks: [withoutTallySource] });
    expect(config.looks[0]?.tallySource).toBe("boxes");
  });

  it("rejects duplicate look ids", () => {
    expect(() => parseShowEngineConfig({ ...minimal, looks: [look, { ...look, label: "Other" }] })
    ).toThrow(/duplicate/i);
  });

  it("rejects a non-array looks value", () => {
    expect(() => parseShowEngineConfig({ ...minimal, looks: {} })).toThrow(/looks/);
  });
});

describe("parseShowEngineConfig gallery dimensions", () => {
  it("defaults galleryCells to sixteen", () => {
    expect(parseShowEngineConfig(minimal).galleryCells).toBe(16);
  });

  it("keeps an explicit galleryCells", () => {
    expect(parseShowEngineConfig({ ...minimal, galleryCells: 9 }).galleryCells).toBe(9);
  });

  it("rejects a galleryCells below one", () => {
    expect(() => parseShowEngineConfig({ ...minimal, galleryCells: 0 })).toThrow(
      /galleryCells/
    );
  });

  it("rejects a non-integer galleryCells", () => {
    expect(() => parseShowEngineConfig({ ...minimal, galleryCells: 4.5 })).toThrow(
      /galleryCells/
    );
  });
});
