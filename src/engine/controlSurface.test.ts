import { describe, expect, it } from "vitest";
import {
  controlButtonStatus,
  controlSurface,
  resolveControlAction,
  summarizeControlSurface,
  type ControlSurfaceContext
} from "./controlSurface";

const context: ControlSurfaceContext = {
  recording: false,
  streaming: false,
  automation: false,
  inMeeting: true
};

describe("controlSurface", () => {
  it("maps six buttons with unique ids and actions", () => {
    expect(controlSurface).toHaveLength(6);
    expect(new Set(controlSurface.map((button) => button.id)).size).toBe(6);
    expect(resolveControlAction("btn-record")).toBe("record");
    expect(resolveControlAction("nope")).toBeUndefined();
  });
});

describe("controlButtonStatus", () => {
  it("lights record and stream when live", () => {
    expect(controlButtonStatus("record", { ...context, recording: true })).toMatchObject({ active: true, stateLabel: "Live" });
    expect(controlButtonStatus("stream", { ...context, streaming: true })).toMatchObject({ active: true, stateLabel: "On air" });
  });

  it("reflects automation state on the set-and-forget button", () => {
    expect(controlButtonStatus("set-and-forget", { ...context, automation: true }).active).toBe(true);
    expect(controlButtonStatus("set-and-forget", context).stateLabel).toBe("Off");
  });

  it("disables Magic Scene when not in a meeting", () => {
    expect(controlButtonStatus("magic-scene", { ...context, inMeeting: false })).toMatchObject({
      disabled: true,
      stateLabel: "Offline"
    });
    expect(controlButtonStatus("magic-scene", context).disabled).toBe(false);
  });
});

describe("summarizeControlSurface", () => {
  it("describes the surface", () => {
    expect(summarizeControlSurface()).toBe("6 buttons mapped - Stream Deck / Companion ready");
  });
});
