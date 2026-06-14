import { describe, expect, it } from "vitest";
import { parseZoomMeetingJoinInput } from "./zoomMeetingInput";

describe("parseZoomMeetingJoinInput", () => {
  it("accepts raw numeric meeting IDs", () => {
    expect(parseZoomMeetingJoinInput(" 123 456 789 ")).toEqual({ meetingNumber: "123456789" });
  });

  it("extracts meeting numbers from common Zoom URLs", () => {
    expect(parseZoomMeetingJoinInput("https://zoom.us/j/987654321")).toEqual({ meetingNumber: "987654321" });
    expect(parseZoomMeetingJoinInput("https://zoom.us/wc/join/1234567890")).toEqual({ meetingNumber: "1234567890" });
  });

  it("preserves passcodes from URL parameters", () => {
    expect(parseZoomMeetingJoinInput("https://zoom.us/j/987654321?pwd=abc123")).toEqual({
      meetingNumber: "987654321",
      passcode: "abc123"
    });
  });

  it("rejects input without a meeting number", () => {
    expect(parseZoomMeetingJoinInput("https://zoom.us/profile")).toBeUndefined();
    expect(parseZoomMeetingJoinInput("not a meeting")).toBeUndefined();
  });
});
