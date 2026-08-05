import { describe, expect, it } from "vitest";
import { ProgramBus } from "./programBus.js";

describe("ProgramBus", () => {
  it("starts black on both buses with follow off", () => {
    const bus = new ProgramBus();
    expect(bus.state()).toEqual({
      program: { kind: "black" },
      preview: { kind: "black" },
      activeSpeakerFollow: false,
      activeSpeakerId: null
    });
  });

  it("sets preview without touching program", () => {
    const bus = new ProgramBus();
    bus.setPreview({ kind: "gallery" });
    expect(bus.state().preview).toEqual({ kind: "gallery" });
    expect(bus.state().program).toEqual({ kind: "black" });
  });

  it("swaps the buses on cut", () => {
    const bus = new ProgramBus();
    bus.setPreview({ kind: "gallery" });
    bus.cut();
    expect(bus.state().program).toEqual({ kind: "gallery" });
    expect(bus.state().preview).toEqual({ kind: "black" });
  });

  it("swaps the buses on auto, identically to cut", () => {
    const bus = new ProgramBus();
    bus.setPreview({ kind: "look", lookId: "banter" });
    bus.auto();
    expect(bus.state().program).toEqual({ kind: "look", lookId: "banter" });
    expect(bus.state().preview).toEqual({ kind: "black" });
  });

  it("drops the outgoing program to preview on a direct cut", () => {
    const bus = new ProgramBus();
    bus.setPreview({ kind: "gallery" });
    bus.cut();
    bus.directCut({ kind: "slot", slot: 3 });
    expect(bus.state().program).toEqual({ kind: "slot", slot: 3 });
    expect(bus.state().preview).toEqual({ kind: "gallery" });
  });

  it("does not move the buses when follow is switched on", () => {
    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);
    expect(bus.state().program).toEqual({ kind: "black" });
    expect(bus.state().activeSpeakerFollow).toBe(true);
  });

  it("takes the active-speaker source when follow is on", () => {
    const bus = new ProgramBus();
    bus.setPreview({ kind: "gallery" });
    bus.cut();
    bus.setActiveSpeakerFollow(true);
    expect(bus.onActiveSpeaker("z1", "panelist")).toBe(true);
    expect(bus.state().program).toEqual({ kind: "activeSpeaker" });
    expect(bus.state().preview).toEqual({ kind: "gallery" });
    expect(bus.state().activeSpeakerId).toBe("z1");
  });

  it("does not re-take when already on the active-speaker source", () => {
    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);
    bus.onActiveSpeaker("z1", "panelist");
    expect(bus.onActiveSpeaker("z2", "panelist")).toBe(false);
    expect(bus.state().program).toEqual({ kind: "activeSpeaker" });
    expect(bus.state().activeSpeakerId).toBe("z2");
  });

  it("tracks the speaker but never cuts when follow is off", () => {
    const bus = new ProgramBus();
    expect(bus.onActiveSpeaker("z1", "panelist")).toBe(false);
    expect(bus.state().activeSpeakerId).toBe("z1");
    expect(bus.state().program).toEqual({ kind: "black" });
  });

  it("ignores a skipped role entirely", () => {
    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);
    bus.onActiveSpeaker("z1", "panelist");
    expect(bus.onActiveSpeaker("interpreter", "aslinterpreter")).toBe(false);
    expect(bus.state().activeSpeakerId).toBe("z1");
  });

  it("does not let a skipped role take program from black", () => {
    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);
    expect(bus.onActiveSpeaker("interpreter", "aslinterpreter")).toBe(false);
    expect(bus.state().program).toEqual({ kind: "black" });
    expect(bus.state().activeSpeakerId).toBeNull();
  });

  it("honours a configured skip list", () => {
    const bus = new ProgramBus({ skipRoles: ["reader"] });
    bus.setActiveSpeakerFollow(true);
    expect(bus.onActiveSpeaker("r", "reader")).toBe(false);
    expect(bus.onActiveSpeaker("i", "aslinterpreter")).toBe(true);
    expect(bus.state().activeSpeakerId).toBe("i");
  });

  it("returns a fresh state object", () => {
    const bus = new ProgramBus();
    const state = bus.state();
    state.program = { kind: "gallery" };
    expect(bus.state().program).toEqual({ kind: "black" });
  });
});
