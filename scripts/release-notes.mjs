// Prints the CHANGELOG section for a tagged release (e.g. v0.1.1 -> 0.1.1).
import { readFileSync } from "node:fs";
import { join } from "node:path";

const tag = process.argv[2]?.trim() ?? "";
const version = tag.replace(/^v/i, "");
if (!version) {
  console.error("Usage: node scripts/release-notes.mjs <tag>");
  process.exit(1);
}

const changelog = readFileSync(join(import.meta.dirname, "..", "CHANGELOG.md"), "utf8");
const marker = `## [${version}]`;
const start = changelog.indexOf(marker);
if (start < 0) {
  console.error(`No CHANGELOG section found for ${version}`);
  process.exit(1);
}

const after = changelog.slice(start + marker.length);
const end = after.search(/\n## \[/);
const body = (end < 0 ? after : after.slice(0, end)).trim();
process.stdout.write(body ? `${body}\n` : `Release ${version}\n`);