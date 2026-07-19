// Tests for scripts/make-appinstaller.mjs (D4 update channel). Exercises the
// real CLI end-to-end (spawned node process) into a temp dir, plus the pure
// builders. The D5 release pipeline calls this exact contract:
//   node scripts/make-appinstaller.mjs --version x.y.z --msix-url <https>
//     --appinstaller-url <https> --output <path> [--latest-json <path>]
//     [--msix-path <path>]
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { afterAll, describe, expect, it } from "vitest";

import { buildAppInstallerXml, buildLatestJson, readIdentityFromManifest } from "./make-appinstaller.mjs";

const SCRIPT = resolve(__dirname, "make-appinstaller.mjs");
const MANIFEST = resolve(__dirname, "..", "native-shell", "CoreVideoPro.WinUI", "Package.appxmanifest");

const workDir = mkdtempSync(join(tmpdir(), "make-appinstaller-"));
afterAll(() => rmSync(workDir, { recursive: true, force: true }));

function runCli(args) {
  try {
    const stdout = execFileSync(process.execPath, [SCRIPT, ...args], { encoding: "utf8" });
    return { code: 0, stdout, stderr: "" };
  } catch (error) {
    return {
      code: error.status ?? 1,
      stdout: error.stdout?.toString() ?? "",
      stderr: error.stderr?.toString() ?? "",
    };
  }
}

function parseXml(xml) {
  const doc = new DOMParser().parseFromString(xml, "text/xml");
  expect(doc.querySelector("parsererror")).toBeNull();
  return doc;
}

describe("make-appinstaller CLI", () => {
  it("emits a well-formed .appinstaller with identity from the real manifest", () => {
    const output = join(workDir, "CoreVideoPro.appinstaller");
    const result = runCli([
      "--version", "1.2.3",
      "--msix-url", "https://updates.example.com/CoreVideoPro-1.2.3.msix",
      "--appinstaller-url", "https://updates.example.com/CoreVideoPro.appinstaller",
      "--output", output,
    ]);
    expect(result.stderr).toBe("");
    expect(result.code).toBe(0);

    const xml = readFileSync(output, "utf8");
    const doc = parseXml(xml);
    const root = doc.documentElement;
    expect(root.tagName).toBe("AppInstaller");
    expect(root.namespaceURI).toBe("http://schemas.microsoft.com/appx/appinstaller/2018");
    expect(root.getAttribute("Version")).toBe("1.2.3.0");
    expect(root.getAttribute("Uri")).toBe("https://updates.example.com/CoreVideoPro.appinstaller");

    const identity = readIdentityFromManifest(readFileSync(MANIFEST, "utf8"));
    expect(identity.error).toBeUndefined();
    const main = root.getElementsByTagName("MainPackage")[0];
    expect(main.getAttribute("Name")).toBe(identity.name);
    expect(main.getAttribute("Publisher")).toBe(identity.publisher);
    expect(main.getAttribute("Version")).toBe("1.2.3.0");
    expect(main.getAttribute("ProcessorArchitecture")).toBe("x64");
    expect(main.getAttribute("Uri")).toBe("https://updates.example.com/CoreVideoPro-1.2.3.msix");

    const onLaunch = root.getElementsByTagName("OnLaunch")[0];
    expect(onLaunch.getAttribute("HoursBetweenUpdateChecks")).toBe("0");
  });

  it("emits latest.json with sha256 when --msix-path is provided", () => {
    const fakeMsix = join(workDir, "fake.msix");
    writeFileSync(fakeMsix, "not-really-an-msix-but-hashable");
    const expectedSha = createHash("sha256").update(readFileSync(fakeMsix)).digest("hex");

    const output = join(workDir, "with-latest.appinstaller");
    const latest = join(workDir, "latest.json");
    const result = runCli([
      "--version", "2.0.0",
      "--msix-url", "https://updates.example.com/CoreVideoPro-2.0.0.msix",
      "--appinstaller-url", "https://updates.example.com/CoreVideoPro.appinstaller",
      "--output", output,
      "--latest-json", latest,
      "--msix-path", fakeMsix,
    ]);
    expect(result.code).toBe(0);

    const parsed = JSON.parse(readFileSync(latest, "utf8"));
    expect(parsed).toEqual({
      version: "2.0.0",
      msixUrl: "https://updates.example.com/CoreVideoPro-2.0.0.msix",
      appinstallerUrl: "https://updates.example.com/CoreVideoPro.appinstaller",
      sha256: expectedSha,
    });
  });

  it("omits sha256 from latest.json when no --msix-path is given", () => {
    const output = join(workDir, "no-hash.appinstaller");
    const latest = join(workDir, "latest-no-hash.json");
    const result = runCli([
      "--version", "2.1.0",
      "--msix-url", "https://updates.example.com/a.msix",
      "--appinstaller-url", "https://updates.example.com/a.appinstaller",
      "--output", output,
      "--latest-json", latest,
    ]);
    expect(result.code).toBe(0);
    const parsed = JSON.parse(readFileSync(latest, "utf8"));
    expect(parsed.sha256).toBeUndefined();
    expect(parsed.version).toBe("2.1.0");
  });

  it.each([
    [["--msix-url", "https://x/a.msix", "--appinstaller-url", "https://x/a.appinstaller", "--output", "o"], /--version is required/],
    [["--version", "1.2", "--msix-url", "https://x/a.msix", "--appinstaller-url", "https://x/a.appinstaller", "--output", "o"], /--version must be x\.y\.z/],
    [["--version", "1.2.3.4", "--msix-url", "https://x/a.msix", "--appinstaller-url", "https://x/a.appinstaller", "--output", "o"], /--version must be x\.y\.z/],
    [["--version", "1.2.3", "--msix-url", "http://x/a.msix", "--appinstaller-url", "https://x/a.appinstaller", "--output", "o"], /--msix-url must be an https/],
    [["--version", "1.2.3", "--msix-url", "https://x/a.msix", "--appinstaller-url", "not a url", "--output", "o"], /--appinstaller-url is not a valid URL/],
    [["--version", "1.2.3", "--msix-url", "https://x/a.msix", "--appinstaller-url", "https://x/a.appinstaller"], /--output is required/],
    [["--version", "1.2.3", "--msix-url", "https://x/a.msix", "--appinstaller-url", "https://x/a.appinstaller", "--output", "o", "--msix-path", join(workDir, "missing.msix")], /--msix-path does not exist/],
    [["--bogus", "value"], /unknown flag --bogus/],
  ])("fails non-zero with a clear message: %j", (args, messagePattern) => {
    const result = runCli(args);
    expect(result.code).not.toBe(0);
    expect(result.stderr).toMatch(messagePattern);
  });
});

describe("builders (pure)", () => {
  it("XML-escapes identity values and URLs", () => {
    const xml = buildAppInstallerXml({
      name: "App<Name>",
      publisher: 'CN="Corp" & Sons',
      version: "1.0.0",
      msixUrl: "https://x/a.msix?a=1&b=2",
      appInstallerUrl: "https://x/a.appinstaller",
    });
    const doc = parseXml(xml);
    const main = doc.getElementsByTagName("MainPackage")[0];
    expect(main.getAttribute("Name")).toBe("App<Name>");
    expect(main.getAttribute("Publisher")).toBe('CN="Corp" & Sons');
    expect(main.getAttribute("Uri")).toBe("https://x/a.msix?a=1&b=2");
  });

  it("reads Identity Name/Publisher from manifest XML", () => {
    const identity = readIdentityFromManifest(
      '<Package><Identity Name="My.App" Publisher="CN=Real &amp; Co" Version="1.0.0.0" /></Package>',
    );
    expect(identity).toEqual({ name: "My.App", publisher: "CN=Real & Co" });
  });

  it("reports missing Identity", () => {
    expect(readIdentityFromManifest("<Package />").error).toMatch(/no <Identity>/);
  });

  it("builds latest.json in the shape the shell consumes", () => {
    const json = JSON.parse(
      buildLatestJson({
        version: "3.1.4",
        msixUrl: "https://x/a.msix",
        appInstallerUrl: "https://x/a.appinstaller",
        sha256: "abc123",
      }),
    );
    expect(json).toEqual({
      version: "3.1.4",
      msixUrl: "https://x/a.msix",
      appinstallerUrl: "https://x/a.appinstaller",
      sha256: "abc123",
    });
  });
});
