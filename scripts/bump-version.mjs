// Bumps package.json semver and rolls CHANGELOG [Unreleased] into a dated section.
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const root = join(import.meta.dirname, "..");
const packagePath = join(root, "package.json");
const changelogPath = join(root, "CHANGELOG.md");
const level = process.argv[2]?.trim();

if (!level || !/^(major|minor|patch)$/.test(level)) {
  console.error("Usage: node scripts/bump-version.mjs <major|minor|patch>");
  process.exit(1);
}

const pkg = JSON.parse(readFileSync(packagePath, "utf8"));
const [major, minor, patch] = pkg.version.split(".").map((part) => Number(part));
const next =
  level === "major"
    ? `${major + 1}.0.0`
    : level === "minor"
      ? `${major}.${minor + 1}.0`
      : `${major}.${minor}.${patch + 1}`;

pkg.version = next;
writeFileSync(packagePath, `${JSON.stringify(pkg, null, 2)}\n`);

const changelog = readFileSync(changelogPath, "utf8");
const today = new Date().toISOString().slice(0, 10);
const released = changelog.replace(
  "## [Unreleased]",
  `## [Unreleased]\n\n### Added\n\n- \n\n## [${next}] - ${today}`
);
writeFileSync(changelogPath, released);

console.info(`[bump] ${pkg.name} -> ${next}`);