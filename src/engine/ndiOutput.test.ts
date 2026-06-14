import { describe, expect, it } from "vitest";
import {
  describeNdiSource,
  estimateNdiBandwidth,
  ndiReceiverSuggestions,
  parseNdiSourceName,
} from "./ndiOutput";

describe("parseNdiSourceName", () => {
  it("rejects empty input", () => {
    const result = parseNdiSourceName("");
    expect(result.valid).toBe(false);
    expect(result.errors[0]).toMatch(/required/i);
  });

  it("rejects names that are too long", () => {
    const result = parseNdiSourceName("A".repeat(65));
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes("64 characters"))).toBe(true);
  });

  it("rejects names with invalid characters", () => {
    const result = parseNdiSourceName("My<Source>");
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes("invalid characters"))).toBe(true);
  });

  it("accepts a plain program name and adds machine prefix", () => {
    const result = parseNdiSourceName("Program");
    expect(result.valid).toBe(true);
    expect(result.sourceName?.machineName).toBe("COREVIDEO");
    expect(result.sourceName?.programName).toBe("Program");
    expect(result.sourceName?.canonical).toBe("COREVIDEO (Program)");
    expect(result.warnings.some((w) => w.includes("COREVIDEO"))).toBe(true);
  });

  it("parses a canonical machine-name form", () => {
    const result = parseNdiSourceName("STUDIO-MAC (Main Program)");
    expect(result.valid).toBe(true);
    expect(result.sourceName?.machineName).toBe("STUDIO-MAC");
    expect(result.sourceName?.programName).toBe("Main Program");
    expect(result.sourceName?.canonical).toBe("STUDIO-MAC (Main Program)");
    expect(result.warnings).toHaveLength(0);
  });

  it("normalises machine name to uppercase", () => {
    const result = parseNdiSourceName("studio-mac (Preview)");
    expect(result.sourceName?.machineName).toBe("STUDIO-MAC");
  });

  it("warns on short program names", () => {
    const result = parseNdiSourceName("A");
    expect(result.valid).toBe(true);
    expect(result.warnings.some((w) => w.includes("Short NDI"))).toBe(true);
  });

  it("accepts conventional 'Program' and 'Preview' names without warnings", () => {
    const prog = parseNdiSourceName("MACHINE (Program)");
    const prev = parseNdiSourceName("MACHINE (Preview)");
    expect(prog.valid).toBe(true);
    expect(prev.valid).toBe(true);
    expect(prog.warnings).toHaveLength(0);
  });
});

describe("estimateNdiBandwidth", () => {
  it("estimates ~125 Mbps for 1080p60", () => {
    const status = estimateNdiBandwidth("1920x1080", 60);
    expect(status.estimatedBandwidthMbps).toBe(125);
    expect(status.bandwidthWarning).toBe(true);
    expect(status.summary).toContain("Gigabit");
  });

  it("estimates lower bandwidth for 1080p30", () => {
    const status = estimateNdiBandwidth("1920x1080", 30);
    expect(status.estimatedBandwidthMbps).toBeLessThan(125);
  });

  it("estimates ~500 Mbps for 4K60", () => {
    const status = estimateNdiBandwidth("3840x2160", 60);
    expect(status.estimatedBandwidthMbps).toBe(500);
    expect(status.bandwidthWarning).toBe(true);
  });

  it("detects 4K from resolution string", () => {
    expect(estimateNdiBandwidth("4096x2160", 60).estimatedBandwidthMbps).toBe(500);
  });

  it("marks as discoverable", () => {
    expect(estimateNdiBandwidth("1920x1080", 60).discoverable).toBe(true);
  });
});

describe("describeNdiSource", () => {
  it("formats source description", () => {
    const desc = describeNdiSource({ machineName: "STUDIO", programName: "Program", canonical: "STUDIO (Program)" });
    expect(desc).toContain("STUDIO (Program)");
    expect(desc).toContain("local network");
  });
});

describe("ndiReceiverSuggestions", () => {
  it("returns a non-empty list of receivers", () => {
    const suggestions = ndiReceiverSuggestions();
    expect(suggestions.length).toBeGreaterThan(0);
    expect(suggestions.some((s) => s.toLowerCase().includes("ndi"))).toBe(true);
  });
});
