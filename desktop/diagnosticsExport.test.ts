import { mkdtemp, readFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { afterEach, describe, expect, it } from "vitest";
import { initialProduction } from "../src/domain/production.ts";
import { createSupportBundle } from "../src/engine/supportBundle.ts";
import { writeSupportBundleExport } from "./diagnosticsExport.ts";

describe("writeSupportBundleExport", () => {
  let tempDir = "";

  afterEach(() => {
    tempDir = "";
  });

  it("writes a redacted JSON support bundle to disk", async () => {
    tempDir = await mkdtemp(join(tmpdir(), "corevideo-diagnostics-"));
    const bundle = createSupportBundle(initialProduction, undefined, { platform: "win32" });
    const exported = await writeSupportBundleExport(tempDir, bundle);

    expect(exported.path).toContain(bundle.id);
    expect(exported.bytes).toBeGreaterThan(0);

    const raw = await readFile(exported.path, "utf8");
    const parsed = JSON.parse(raw) as { app: { platform: string } };
    expect(parsed.app.platform).toBe("win32");
    expect(raw).toContain("AI Product Launch Webinar");
  });
});