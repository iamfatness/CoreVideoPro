// One version, stamped everywhere (docs/beta-engineering-spec.md §D1).
//
// package.json `version` is the SINGLE source of truth. This script rewrites:
//   - native-shell/CoreVideoPro.WinUI/Package.appxmanifest  <Identity Version="x.y.z.0">
//   - native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj
//       <ApplicationDisplayVersion>x.y.z</ApplicationDisplayVersion>
//       <ApplicationVersion>major*10000 + minor*100 + patch</ApplicationVersion>
//         (monotonic build integer; 0.1.0 -> 100 — matches the pre-stamp value)
//
// Modes:
//   node scripts/stamp-version.mjs           stamp (idempotent; only rewrites files
//                                            whose value differs)
//   node scripts/stamp-version.mjs --check   CI mode: exit 1 with a diff-style
//                                            report when any source is out of sync
//
// Zero dependencies (plain Node >= 20). Edits are targeted string replacements —
// never a full XML reformat — so encoding, line endings, and surrounding content
// are preserved byte-for-byte.
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const root = join(import.meta.dirname, "..");
const checkMode = process.argv.includes("--check");

const pkg = JSON.parse(readFileSync(join(root, "package.json"), "utf8"));
const version = pkg.version;
if (!/^\d+\.\d+\.\d+$/.test(version)) {
  console.error(`[stamp] package.json version "${version}" is not plain x.y.z semver; refusing to stamp.`);
  process.exit(1);
}

const [major, minor, patch] = version.split(".").map(Number);
if (minor > 99 || patch > 99) {
  // The ApplicationVersion encoding below allots two digits per component.
  console.error(`[stamp] version ${version} has a minor/patch component > 99; the ApplicationVersion encoding (major*10000 + minor*100 + patch) would collide. Pick a version with components <= 99.`);
  process.exit(1);
}
const msixVersion = `${version}.0`;
const applicationVersion = String(major * 10000 + minor * 100 + patch);

// Each stamper returns { label, current, expected, next } where `next` is the
// full file text with the value replaced (identical to input when in sync).
function stampAppxManifest(text) {
  const identityMatch = text.match(/<Identity\b[^>]*>/);
  if (!identityMatch) {
    throw new Error("no <Identity> element found");
  }
  const identity = identityMatch[0];
  const versionMatch = identity.match(/\bVersion="([^"]*)"/);
  if (!versionMatch) {
    throw new Error("<Identity> element has no Version attribute");
  }
  const nextIdentity = identity.replace(versionMatch[0], `Version="${msixVersion}"`);
  return [{
    label: "Identity Version",
    current: versionMatch[1],
    expected: msixVersion,
    next: text.replace(identity, nextIdentity),
  }];
}

function stampCsproj(text) {
  const edits = [];
  let next = text;
  const elements = [
    { tag: "ApplicationDisplayVersion", expected: version, required: true },
    { tag: "ApplicationVersion", expected: applicationVersion, required: false },
  ];
  for (const { tag, expected, required } of elements) {
    const re = new RegExp(`<${tag}>([^<]*)</${tag}>`);
    const m = next.match(re);
    if (!m) {
      if (required) {
        throw new Error(`no <${tag}> element found`);
      }
      continue;
    }
    edits.push({ label: tag, current: m[1], expected });
    next = next.replace(m[0], `<${tag}>${expected}</${tag}>`);
  }
  for (const edit of edits) {
    edit.next = next; // csproj edits share one rewritten text
  }
  return edits;
}

const targets = [
  {
    file: "native-shell/CoreVideoPro.WinUI/Package.appxmanifest",
    stamp: stampAppxManifest,
  },
  {
    file: "native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj",
    stamp: stampCsproj,
  },
];

const results = [];
for (const target of targets) {
  const path = join(root, target.file);
  const text = readFileSync(path, "utf8");
  let edits;
  try {
    edits = target.stamp(text);
  } catch (err) {
    console.error(`[stamp] ${target.file}: ${err.message}`);
    process.exit(1);
  }
  for (const edit of edits) {
    results.push({ file: target.file, path, text, ...edit });
  }
}

const outOfSync = results.filter((r) => r.current !== r.expected);

if (checkMode) {
  if (outOfSync.length > 0) {
    console.error(`[stamp --check] version sources OUT OF SYNC (source of truth: package.json = ${version}):`);
    for (const r of outOfSync) {
      console.error(`  ${r.file} (${r.label}):`);
      console.error(`    - ${r.current}`);
      console.error(`    + ${r.expected}`);
    }
    console.error("Fix: npm run stamp:version   (node scripts/stamp-version.mjs), then commit the result.");
    process.exit(1);
  }
  console.info(`[stamp --check] all version sources agree with package.json (${version}).`);
  process.exit(0);
}

const rewritten = new Set();
for (const r of results) {
  if (r.current === r.expected) {
    console.info(`[stamp] ${r.file} (${r.label}): ${r.current} (already in sync)`);
    continue;
  }
  if (!rewritten.has(r.path)) {
    writeFileSync(r.path, r.next);
    rewritten.add(r.path);
  }
  console.info(`[stamp] ${r.file} (${r.label}): ${r.current} -> ${r.expected}`);
}
console.info(`[stamp] done. package.json ${version} -> Identity ${msixVersion}, ApplicationDisplayVersion ${version}, ApplicationVersion ${applicationVersion}.`);
