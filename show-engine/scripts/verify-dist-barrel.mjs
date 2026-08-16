#!/usr/bin/env node
/**
 * Verify the package's public surface against the BUILT `dist`, not the
 * source — the check `npm test` structurally cannot make.
 *
 * Why this is a script and not a vitest test (Task 10, fix round 1): `npm
 * test` does not build, so a test that imported `./dist/index.js` would
 * either fail on a clean checkout or "skip when dist is missing", which is
 * a guard that looks like coverage and isn't. `src/*.test.ts` resolves
 * names through `./index.js` — the SOURCE barrel — which proves the export
 * line exists but not that the name survives compilation and reaches a
 * consumer of `dist/index.js` + `dist/index.d.ts`. A previous plan found
 * several names unreachable despite having export lines written.
 *
 * Run with `npm run verify:barrel` (which builds first). Three checks:
 *
 *  1. Every runtime value resolves from the built `dist/index.js`.
 *  2. Every type-only name resolves from the built `dist/index.d.ts`, by
 *     compiling a generated file that uses each in a type position under
 *     `--strict --module nodenext`.
 *  3. `HOST_CONFORMANCE_CASES` actually EXECUTES from `dist`, with no test
 *     framework loaded, against three host shapes: the default recorder, a
 *     preview-less/8-cell host, and a key-reordered recording facade (the
 *     shape a Plan 7-9 adapter's own recorder has — see `canonicalize` in
 *     `conformance.ts`).
 */

import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const packageRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const distEntry = path.join(packageRoot, "dist", "index.js");

/** Runtime values a host bridge (Companion, OSC, the three native panels) needs. */
const RUNTIME_NAMES = [
  "OHG_ACTIONS",
  "invokeAction",
  "oscAddressFor",
  "parseProgramSource",
  "formatProgramSource",
  "projectControlFields",
  "OHG_FIELD_TEMPLATES",
  "HOST_CONFORMANCE_CASES",
  "CONFORMANCE_CONFIG",
  "CONFORMANCE_LOOK_ID",
  "CONFORMANCE_SCENE_PRESET",
  "MukanaPoller",
  "LookController",
  "MockHost",
  "ShowEngine",
  "StateStore"
];

/** Type-only names, each used in a type position by the generated file below. */
const TYPE_CHECK_SOURCE = `
import {
  type ActionDefinition,
  type ActionParam,
  type ActionParamType,
  type ActionResult,
  type ConformanceCase,
  type ConformanceHost,
  type ControlFieldValue,
  type Headline,
  type LookPlacement
} from "DIST_SPECIFIER";

const paramType: ActionParamType = "string";
const param: ActionParam = { name: "n", type: paramType, required: true, description: "" };
const definition: ActionDefinition = { id: "x", title: "x", description: "", params: [param] };
const result: ActionResult = { kind: "refused", reason: "because" };
const field: ControlFieldValue = null;
const headline: Headline = { name: "n", location: "l" };
const placement: LookPlacement = {
  lookId: "l",
  scenePreset: "p",
  hostSlot: null,
  readerSlot: null,
  boxes: new Map<number, number | null>([[1, 3]])
};
declare const conformanceCase: ConformanceCase;
declare const host: ConformanceHost;

export const used = [definition, result, field, headline, placement, conformanceCase.name, host.calls()];
`;

function fail(message) {
  console.error(`verify-dist-barrel: ${message}`);
  process.exitCode = 1;
}

/** An in-memory `StateFs` — the suite's runner contract requires one (its `statePath` is a root path). */
function memoryFs() {
  const files = new Map();
  return {
    readFile: async (p) => {
      const value = files.get(p);
      if (value === undefined) throw new Error(`ENOENT ${p}`);
      return value;
    },
    writeFile: async (p, contents) => void files.set(p, contents),
    rename: async (from, to) => {
      const value = files.get(from);
      if (value !== undefined) {
        files.set(to, value);
        files.delete(from);
      }
    },
    mkdir: async () => undefined
  };
}

/** Rebuild `value` with object keys in reverse order, recursively — a stand-in for a foreign recorder. */
function reorderKeys(value) {
  if (Array.isArray(value)) return value.map(reorderKeys);
  if (value === null || typeof value !== "object") return value;
  return Object.fromEntries(
    Object.entries(value)
      .map(([key, entry]) => [key, reorderKeys(entry)])
      .reverse()
  );
}

async function main() {
  const barrel = await import(distEntry);

  // 1. Runtime names.
  const missing = RUNTIME_NAMES.filter((name) => barrel[name] === undefined);
  if (missing.length > 0) fail(`unreachable from dist/index.js: ${missing.join(", ")}`);
  else console.log(`OK  ${RUNTIME_NAMES.length} runtime names resolve from dist/index.js`);

  // 2. Type-only names, against the emitted .d.ts.
  const tempDir = mkdtempSync(path.join(tmpdir(), "cvp-barrel-"));
  try {
    const specifier = path
      .relative(tempDir, distEntry)
      .split(path.sep)
      .join("/");
    const checkFile = path.join(tempDir, "barrel-types.ts");
    writeFileSync(checkFile, TYPE_CHECK_SOURCE.replace("DIST_SPECIFIER", specifier));
    execFileSync(
      "npx",
      [
        "tsc",
        "--noEmit",
        "--strict",
        "--module",
        "nodenext",
        "--moduleResolution",
        "nodenext",
        "--target",
        "es2022",
        "--skipLibCheck",
        checkFile
      ],
      { cwd: packageRoot, stdio: "pipe" }
    );
    console.log("OK  every type-only name resolves from dist/index.d.ts");
  } catch (error) {
    const output = [error.stdout, error.stderr].filter(Boolean).map(String).join("\n").trim();
    fail(`type-only names failed to resolve from dist/index.d.ts:\n${output || error.message}`);
  } finally {
    rmSync(tempDir, { recursive: true, force: true });
  }

  // 3. The conformance suite, executed from dist against three host shapes.
  const { HOST_CONFORMANCE_CASES, CONFORMANCE_CONFIG, MockHost, ShowEngine, StateStore } = barrel;

  // Check 3 needs the names check 1 just verified. When one is missing, say
  // so and stop — an unreachable export must produce this script's own
  // diagnostic, not a `TypeError` from using `undefined` a few lines later.
  if (!Array.isArray(HOST_CONFORMANCE_CASES) || MockHost === undefined || ShowEngine === undefined) {
    fail("skipping the conformance run — the names it needs are not reachable from dist/index.js");
    console.error("verify-dist-barrel: FAILED");
    return;
  }

  class KeyReorderingHost extends MockHost {
    calls() {
      return super.calls().map(reorderKeys);
    }
    callsOfKind(kind) {
      return super.callsOfKind(kind).map(reorderKeys);
    }
  }

  const shapes = [
    { label: "default recorder", make: () => new MockHost() },
    {
      label: "preview-less, 8-cell host",
      make: () => new MockHost({ hasPreviewBus: false, maxGalleryCells: 8, transitions: [] })
    },
    { label: "key-reordered recording facade", make: () => new KeyReorderingHost() }
  ];

  for (const shape of shapes) {
    let passed = 0;
    for (const conformanceCase of HOST_CONFORMANCE_CASES) {
      const host = shape.make();
      const engine = new ShowEngine({
        config: CONFORMANCE_CONFIG,
        host,
        clock: { now: () => 1000 },
        store: new StateStore(CONFORMANCE_CONFIG.statePath, { fs: memoryFs() })
      });
      try {
        // A no-op flush on purpose: the runner contract says the drain is
        // currently unexercised, and this is what keeps that claim honest.
        await conformanceCase.run(engine, host, async () => {});
        passed += 1;
      } catch (error) {
        fail(`[${shape.label}] ${error instanceof Error ? error.message : String(error)}`);
      }
    }
    if (passed === HOST_CONFORMANCE_CASES.length) {
      console.log(`OK  ${passed}/${HOST_CONFORMANCE_CASES.length} conformance cases pass from dist — ${shape.label}`);
    }
  }

  if (process.exitCode === 1) {
    console.error("verify-dist-barrel: FAILED");
  } else {
    console.log("verify-dist-barrel: OK");
  }
}

await main();
