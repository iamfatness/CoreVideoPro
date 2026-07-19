#!/usr/bin/env node
// make-appinstaller.mjs — D4 update channel: emit the .appinstaller XML (and
// optionally latest.json) for a released MSIX. Called by the D5 release
// pipeline with:
//
//   node scripts/make-appinstaller.mjs \
//     --version <x.y.z> \
//     --msix-url <https-url-to-msix> \
//     --appinstaller-url <https-url-where-this-file-will-live> \
//     --output <path> \
//     [--latest-json <path>] [--msix-path <path-to-msix-for-sha256>]
//
// Identity Name/Publisher are read from the real Package.appxmanifest (never
// hard-coded) so a D2 publisher change flows through automatically. Version is
// emitted as x.y.z.0 (MSIX four-part form). The shell's startup version check
// (UpdateCheckService) consumes the latest.json shape emitted here.
//
// Plain Node, no dependencies. Fails non-zero with a clear message on any
// invalid input — a release pipeline must never publish a malformed feed.

import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
const MANIFEST_PATH = resolve(
  SCRIPT_DIR,
  "..",
  "native-shell",
  "CoreVideoPro.WinUI",
  "Package.appxmanifest",
);

function fail(message) {
  console.error(`make-appinstaller: ${message}`);
  process.exit(1);
}

function parseArgs(argv) {
  const args = {};
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (!arg.startsWith("--")) {
      fail(`unexpected argument "${arg}" (flags only, e.g. --version 1.2.3)`);
    }
    const key = arg.slice(2);
    const known = [
      "version",
      "msix-url",
      "appinstaller-url",
      "output",
      "latest-json",
      "msix-path",
    ];
    if (!known.includes(key)) {
      fail(`unknown flag --${key} (known: ${known.map((k) => `--${k}`).join(", ")})`);
    }
    const value = argv[i + 1];
    if (value === undefined || value.startsWith("--")) {
      fail(`flag --${key} requires a value`);
    }
    args[key] = value;
    i += 1;
  }
  return args;
}

function requireVersion(text) {
  if (!text) fail("--version is required (x.y.z)");
  if (!/^\d+\.\d+\.\d+$/.test(text)) {
    fail(`--version must be x.y.z with numeric parts (got "${text}")`);
  }
  return text;
}

function requireHttpsUrl(flag, text) {
  if (!text) fail(`--${flag} is required`);
  let url;
  try {
    url = new URL(text);
  } catch {
    fail(`--${flag} is not a valid URL (got "${text}")`);
  }
  if (url.protocol !== "https:") {
    fail(`--${flag} must be an https:// URL (App Installer requires https; got "${text}")`);
  }
  return url.href;
}

function xmlEscape(text) {
  return text
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&apos;");
}

export function readIdentityFromManifest(manifestXml) {
  const identityMatch = manifestXml.match(/<Identity\b[^>]*>/);
  if (!identityMatch) {
    return { error: "no <Identity> element found in Package.appxmanifest" };
  }
  const identity = identityMatch[0];
  const name = identity.match(/\bName="([^"]+)"/)?.[1];
  const publisher = identity.match(/\bPublisher="([^"]+)"/)?.[1];
  if (!name || !publisher) {
    return { error: "Identity element is missing Name or Publisher" };
  }
  // Attribute values in the manifest are XML-escaped; decode so we can
  // re-escape consistently on output.
  const decode = (value) =>
    value
      .replaceAll("&lt;", "<")
      .replaceAll("&gt;", ">")
      .replaceAll("&quot;", '"')
      .replaceAll("&apos;", "'")
      .replaceAll("&amp;", "&");
  return { name: decode(name), publisher: decode(publisher) };
}

export function buildAppInstallerXml({ name, publisher, version, msixUrl, appInstallerUrl }) {
  const fourPart = `${version}.0`;
  return `<?xml version="1.0" encoding="utf-8"?>
<AppInstaller
    xmlns="http://schemas.microsoft.com/appx/appinstaller/2018"
    Uri="${xmlEscape(appInstallerUrl)}"
    Version="${fourPart}">
  <MainPackage
      Name="${xmlEscape(name)}"
      Publisher="${xmlEscape(publisher)}"
      Version="${fourPart}"
      ProcessorArchitecture="x64"
      Uri="${xmlEscape(msixUrl)}" />
  <UpdateSettings>
    <OnLaunch HoursBetweenUpdateChecks="0" />
  </UpdateSettings>
</AppInstaller>
`;
}

export function buildLatestJson({ version, msixUrl, appInstallerUrl, sha256 }) {
  const payload = {
    version,
    msixUrl,
    appinstallerUrl: appInstallerUrl,
  };
  if (sha256) {
    payload.sha256 = sha256;
  }
  return `${JSON.stringify(payload, null, 2)}\n`;
}

function main() {
  const args = parseArgs(process.argv.slice(2));

  const version = requireVersion(args.version);
  const msixUrl = requireHttpsUrl("msix-url", args["msix-url"]);
  const appInstallerUrl = requireHttpsUrl("appinstaller-url", args["appinstaller-url"]);
  const output = args.output;
  if (!output) fail("--output is required (path for the .appinstaller file)");

  if (!existsSync(MANIFEST_PATH)) {
    fail(`Package.appxmanifest not found at ${MANIFEST_PATH}`);
  }
  const identity = readIdentityFromManifest(readFileSync(MANIFEST_PATH, "utf8"));
  if (identity.error) fail(identity.error);

  let sha256;
  if (args["msix-path"]) {
    const msixPath = resolve(args["msix-path"]);
    if (!existsSync(msixPath)) {
      fail(`--msix-path does not exist: ${msixPath}`);
    }
    sha256 = createHash("sha256").update(readFileSync(msixPath)).digest("hex");
  }

  const xml = buildAppInstallerXml({
    name: identity.name,
    publisher: identity.publisher,
    version,
    msixUrl,
    appInstallerUrl,
  });
  const outputPath = resolve(output);
  mkdirSync(dirname(outputPath), { recursive: true });
  writeFileSync(outputPath, xml, "utf8");
  console.log(
    `make-appinstaller: wrote ${outputPath} (Name=${identity.name}, Publisher=${identity.publisher}, Version=${version}.0)`,
  );

  if (args["latest-json"]) {
    const latestPath = resolve(args["latest-json"]);
    mkdirSync(dirname(latestPath), { recursive: true });
    writeFileSync(latestPath, buildLatestJson({ version, msixUrl, appInstallerUrl, sha256 }), "utf8");
    console.log(
      `make-appinstaller: wrote ${latestPath}${sha256 ? ` (sha256=${sha256.slice(0, 12)}…)` : " (no --msix-path, sha256 omitted)"}`,
    );
  } else if (args["msix-path"]) {
    console.log("make-appinstaller: note — --msix-path given without --latest-json; hash unused");
  }
}

const isDirectRun =
  process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isDirectRun) {
  main();
}
