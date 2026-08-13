# OHG Show Engine — Plan 1: Core Identity & Roster

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `show-engine` package's identity and roster core — turning host participant events plus the Mukana registry into a managed, persistent live-slot roster.

**Architecture:** A new host-agnostic TypeScript workspace package (`show-engine/`, sibling to `native-core/`). Pure logic modules with no I/O; the one network module (`mukanaClient`) and the one filesystem module (`persistence`) take injected dependencies so they stay unit-testable. Data flows one direction: participant events → identity enrichment → Mukana/override join → master panelist DB → live slots.

**Tech Stack:** TypeScript 5.9 (strict, ES2022, NodeNext), vitest 4, Node 24.

**Source documents:**
- Spec: `docs/superpowers/specs/2026-08-04-ohg-show-engine-design.md`
- Algorithm reference: `docs/superpowers/specs/2026-08-04-ohg-isadora-actor-reference.md`

**Plan series (this is Plan 1 of 6):**
1. **Core identity & roster** ← this plan
2. Direction & outputs (speakerRecency, gallery, handsQueue, lookDirector, programBus, tally, gfx, hostAdapter, orchestrator)
3. CVP Windows host integration (subprocess supervision, `ohg.*` control registration, CVP adapter, WinUI panel)
4. mac-shell control server + SwiftUI panel
5. OBS plugin adapter
6. Migration tooling (shadow-mode comparator, config importer)

## Global Constraints

- **Branch:** `spec/ohg-show-engine`. Commit after every task.
- **Module system:** NodeNext. **Every relative import MUST end in `.js`** (e.g. `import { x } from "./contracts.js"`) even though the source file is `.ts`. This matches `native-core/`.
- **vitest runs with `globals: false`** — every test file must explicitly `import { describe, expect, it } from "vitest";`.
- **Strict TypeScript.** No `any`. No non-null assertions (`!`) where a guard will do.
- **No I/O in logic modules.** Network and filesystem access happen only in `mukanaClient.ts` and `persistence.ts`, and both receive their dependencies via constructor injection.
- **Loud, never silent** (repo house rule): failures surface as typed outcomes or thrown errors with actionable messages — never a swallowed exception or a silent default.
- **Do not port the Isadora quirks.** The reference doc's "Reimplementation gotchas" section lists them; the fixed behavior is normative. Specifically: slot keys are plain integers (never `"1-"` / `"1-<zoomID>"`), absent PINs are `null` (never `" "` or `"####"`), and lookups return `null`/`undefined` on miss (never the last-inspected element).
- **Test file placement:** adjacent to the module, `foo.test.ts` beside `foo.ts`.
- **Module header:** every source file opens with a `/** ... */` block comment describing its responsibility, matching `src/engine/` style.

---

## File Structure

```
show-engine/
  package.json            — workspace package manifest
  tsconfig.json           — strict ES2022/NodeNext, mirrors native-core
  vitest.config.ts        — node environment, globals: false
  src/
    contracts.ts          — shared types: Role, Participant, Identity, Panelist, Slot, MukanaRecord
    config.ts             — typed show config, defaults, validation
    identity.ts           — PIN extraction + display-name/location splitting (pure)
    zoomIngest.ts         — participant event cache with published-snapshot double buffer
    mukanaParse.ts        — REST body → PIN-keyed registry; dormant/error gate; persistent merge
    mukanaClient.ts       — polling HTTP client with backoff (injected fetch)
    overrideDb.ts         — operator role overrides with host/reader exclusivity
    panelistDb.ts         — the join: participants × mukana × overrides → master DB (pure)
    liveSlots.ts          — fixed-capacity slot manager (holes, utility tail, role uniqueness)
    persistence.ts        — atomic-write JSON state store (injected fs)
    index.ts              — public exports
```

Modified: `package.json` (root — add the workspace and test script).

---

### Task 1: Package scaffold and typed config

**Files:**
- Create: `show-engine/package.json`
- Create: `show-engine/tsconfig.json`
- Create: `show-engine/vitest.config.ts`
- Create: `show-engine/src/config.ts`
- Test: `show-engine/src/config.test.ts`
- Modify: `package.json` (root — `workspaces` array, add `test:show-engine` script)

**Interfaces:**
- Consumes: nothing (first task).
- Produces: `ShowEngineConfig`, `MukanaConfig`, `parseShowEngineConfig(raw: unknown): ShowEngineConfig`. Later plans extend `ShowEngineConfig` with `spx`, `tally`, `looks`, and `skipRoles` — do not add them now (YAGNI).

- [ ] **Step 1: Create the package scaffold**

`show-engine/package.json`:

```json
{
  "name": "@corevideo/show-engine",
  "version": "0.1.0",
  "private": true,
  "type": "module",
  "main": "dist/index.js",
  "types": "dist/index.d.ts",
  "scripts": {
    "build": "tsc -p tsconfig.json",
    "test": "vitest run",
    "typecheck": "tsc -p tsconfig.json --noEmit"
  },
  "devDependencies": {
    "@types/node": "^24.10.1",
    "typescript": "^5.9.3",
    "vitest": "^4.1.8"
  }
}
```

`show-engine/tsconfig.json`:

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "NodeNext",
    "moduleResolution": "NodeNext",
    "strict": true,
    "declaration": true,
    "outDir": "dist",
    "rootDir": "src",
    "skipLibCheck": true,
    "forceConsistentCasingInFileNames": true
  },
  "include": ["src/**/*.ts"],
  "exclude": ["src/**/*.test.ts"]
}
```

`show-engine/vitest.config.ts`:

```ts
import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    exclude: ["dist/**", "node_modules/**"],
    environment: "node",
    globals: false
  }
});
```

In the root `package.json`, add `"show-engine"` to the `workspaces` array (after `"native-core"`) and add this script beside `test:native-core`:

```json
"test:show-engine": "npm run test --workspace show-engine",
```

Then run `npm install` from the repo root so the workspace links.

- [ ] **Step 2: Write the failing test**

`show-engine/src/config.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { parseShowEngineConfig } from "./config.js";

const minimal = {
  capacity: 16,
  statePath: "/var/tmp/ohg-state.json",
  mukana: { baseUrl: "https://hoka.example.com/php-panel-rest.php", event: "officehours" }
};

describe("parseShowEngineConfig", () => {
  it("applies defaults for omitted optional fields", () => {
    const config = parseShowEngineConfig(minimal);
    expect(config.capacity).toBe(16);
    expect(config.utilityPinBase).toBe(9000);
    expect(config.mukana.panelistsIntervalMs).toBe(5000);
    expect(config.mukana.handsIntervalMs).toBe(2000);
    expect(config.mukana.maxBackoffMs).toBe(60000);
  });

  it("keeps explicitly provided values", () => {
    const config = parseShowEngineConfig({
      ...minimal,
      utilityPinBase: 8000,
      mukana: { ...minimal.mukana, panelistsIntervalMs: 1500 }
    });
    expect(config.utilityPinBase).toBe(8000);
    expect(config.mukana.panelistsIntervalMs).toBe(1500);
  });

  it("rejects a capacity below 1", () => {
    expect(() => parseShowEngineConfig({ ...minimal, capacity: 0 })).toThrow(/capacity/);
  });

  it("rejects a missing mukana baseUrl", () => {
    expect(() => parseShowEngineConfig({ ...minimal, mukana: { event: "officehours" } })).toThrow(
      /mukana\.baseUrl/
    );
  });

  it("rejects a non-object config", () => {
    expect(() => parseShowEngineConfig("nope")).toThrow(/config/);
  });
});
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./config.js`.

- [ ] **Step 4: Write the implementation**

`show-engine/src/config.ts`:

```ts
/**
 * Typed show configuration for the OHG show engine.
 * Replaces the Isadora patch's `infraestructure-*.js` include files: every
 * external address, interval, and capacity lives here, validated at load so a
 * bad config fails loudly at startup instead of mid-show.
 */

export type MukanaConfig = {
  /** Base REST endpoint, e.g. https://host/phpsdk/php-panel-rest.php */
  baseUrl: string;
  /** The `event` query parameter, e.g. "officehours" */
  event: string;
  panelistsIntervalMs: number;
  handsIntervalMs: number;
  questionIntervalMs: number;
  /** Ceiling for exponential backoff after consecutive failures */
  maxBackoffMs: number;
};

export type ShowEngineConfig = {
  /** Number of concurrent participant slots the host can deliver */
  capacity: number;
  /**
   * PINs at or above this value denote utility participants (graphics bots,
   * playback machines) that are pinned to the tail slots rather than the
   * first free slot. `pin - utilityPinBase` is the offset from the last slot.
   */
  utilityPinBase: number;
  mukana: MukanaConfig;
  /** Absolute path of the persisted show-state JSON file */
  statePath: string;
};

const DEFAULT_UTILITY_PIN_BASE = 9000;
const DEFAULT_PANELISTS_INTERVAL_MS = 5000;
const DEFAULT_HANDS_INTERVAL_MS = 2000;
const DEFAULT_QUESTION_INTERVAL_MS = 2000;
const DEFAULT_MAX_BACKOFF_MS = 60000;

function asRecord(value: unknown, field: string): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`show-engine ${field}: expected an object, got ${typeof value}`);
  }
  return value as Record<string, unknown>;
}

/** `label` is the dotted path used in error messages; `key` is the actual property. */
function requireString(source: Record<string, unknown>, key: string, label: string): string {
  const value = source[key];
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new Error(`show-engine ${label}: required non-empty string`);
  }
  return value;
}

function requirePositiveInt(
  source: Record<string, unknown>,
  key: string,
  label: string
): number {
  const value = source[key];
  if (typeof value !== "number" || !Number.isInteger(value) || value < 1) {
    throw new Error(`show-engine ${label}: required integer >= 1, got ${String(value)}`);
  }
  return value;
}

function optionalPositiveInt(
  source: Record<string, unknown>,
  key: string,
  label: string,
  fallback: number
): number {
  const value = source[key];
  if (value === undefined) return fallback;
  if (typeof value !== "number" || !Number.isInteger(value) || value < 1) {
    throw new Error(`show-engine ${label}: expected integer >= 1, got ${String(value)}`);
  }
  return value;
}

/** Validate raw JSON into a ShowEngineConfig, applying defaults. Throws on any invalid field. */
export function parseShowEngineConfig(raw: unknown): ShowEngineConfig {
  const root = asRecord(raw, "config");
  const mukanaRaw = asRecord(root.mukana, "config.mukana");

  return {
    capacity: requirePositiveInt(root, "capacity", "config.capacity"),
    utilityPinBase: optionalPositiveInt(
      root,
      "utilityPinBase",
      "config.utilityPinBase",
      DEFAULT_UTILITY_PIN_BASE
    ),
    statePath: requireString(root, "statePath", "config.statePath"),
    mukana: {
      baseUrl: requireString(mukanaRaw, "baseUrl", "config.mukana.baseUrl"),
      event: requireString(mukanaRaw, "event", "config.mukana.event"),
      panelistsIntervalMs: optionalPositiveInt(
        mukanaRaw,
        "panelistsIntervalMs",
        "config.mukana.panelistsIntervalMs",
        DEFAULT_PANELISTS_INTERVAL_MS
      ),
      handsIntervalMs: optionalPositiveInt(
        mukanaRaw,
        "handsIntervalMs",
        "config.mukana.handsIntervalMs",
        DEFAULT_HANDS_INTERVAL_MS
      ),
      questionIntervalMs: optionalPositiveInt(
        mukanaRaw,
        "questionIntervalMs",
        "config.mukana.questionIntervalMs",
        DEFAULT_QUESTION_INTERVAL_MS
      ),
      maxBackoffMs: optionalPositiveInt(
        mukanaRaw,
        "maxBackoffMs",
        "config.mukana.maxBackoffMs",
        DEFAULT_MAX_BACKOFF_MS
      )
    }
  };
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `npm run test --workspace show-engine`
Expected: PASS — 5 tests.

- [ ] **Step 6: Commit**

```bash
git add show-engine package.json package-lock.json
git commit -m "feat(show-engine): package scaffold and typed show config"
```

---

### Task 2: Shared contracts

**Files:**
- Create: `show-engine/src/contracts.ts`
- Test: `show-engine/src/contracts.test.ts`

**Interfaces:**
- Consumes: nothing.
- Produces: types `Role`, `Participant`, `Identity`, `Panelist`, `Slot`, `MukanaRecord`, `MukanaDb`; constant `ROLES`; guard `isRole(value: unknown): value is Role`; `coerceRole(value: unknown): Role` (falls back to `"panelist"`).

- [ ] **Step 1: Write the failing test**

`show-engine/src/contracts.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { coerceRole, isRole, ROLES } from "./contracts.js";

describe("roles", () => {
  it("lists the five editorial roles", () => {
    expect(ROLES).toEqual([
      "panelist",
      "host",
      "reader",
      "aslpanelist",
      "aslinterpreter"
    ]);
  });

  it("recognises valid roles", () => {
    expect(isRole("host")).toBe(true);
    expect(isRole("aslinterpreter")).toBe(true);
  });

  it("rejects unknown values", () => {
    expect(isRole("moderator")).toBe(false);
    expect(isRole(3)).toBe(false);
    expect(isRole(undefined)).toBe(false);
  });

  it("coerces unknown values to panelist", () => {
    expect(coerceRole("host")).toBe("host");
    expect(coerceRole("moderator")).toBe("panelist");
    expect(coerceRole(undefined)).toBe("panelist");
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./contracts.js`.

- [ ] **Step 3: Write the implementation**

`show-engine/src/contracts.ts`:

```ts
/**
 * Shared data contracts for the OHG show engine.
 * These are the normative shapes from the design spec §5. Note the deliberate
 * divergences from the Isadora patch: an absent PIN is `null` (never the string
 * `" "` or `"####"`), and slots are keyed by plain integers.
 */

export type Role = "panelist" | "host" | "reader" | "aslpanelist" | "aslinterpreter";

export const ROLES: readonly Role[] = [
  "panelist",
  "host",
  "reader",
  "aslpanelist",
  "aslinterpreter"
];

/** Roles that may be held by at most one person at a time. */
export const EXCLUSIVE_ROLES: readonly Role[] = ["host", "reader"];

export function isRole(value: unknown): value is Role {
  return typeof value === "string" && (ROLES as readonly string[]).includes(value);
}

/** Narrow an untrusted value to a Role, defaulting to "panelist". */
export function coerceRole(value: unknown): Role {
  return isRole(value) ? value : "panelist";
}

/** A participant as reported by the host's Zoom engine. */
export type Participant = {
  participantId: string;
  rawName: string;
  online: boolean;
  videoOn: boolean;
  audioOn: boolean;
  handRaised: boolean;
  /** The host's numeric Zoom role. Display only — never the editorial role. */
  zoomRole: number;
};

/** Display identity parsed out of a raw Zoom display name. */
export type Identity = {
  displayName: string;
  location: string;
  /** 4-digit Mukana PIN, or null when the name carries none. */
  pin: string | null;
};

/** A participant joined against the Mukana registry and operator overrides. */
export type Panelist = Participant &
  Identity & {
    hasMukana: boolean;
    role: Role;
  };

/** One position in the fixed-capacity live roster. `panelist === null` is an empty hole. */
export type Slot = {
  slot: number;
  panelist: Panelist | null;
};

/** A Mukana registry record, re-keyed by PIN. */
export type MukanaRecord = {
  pin: string;
  displayName: string;
  location: string;
  role: Role;
  online: boolean;
};

/** The Mukana registry, keyed by 4-digit PIN. */
export type MukanaDb = Record<string, MukanaRecord>;
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/contracts.ts show-engine/src/contracts.test.ts
git commit -m "feat(show-engine): shared data contracts and role helpers"
```

---

### Task 3: Identity extraction

**Files:**
- Create: `show-engine/src/identity.ts`
- Test: `show-engine/src/identity.test.ts`

**Interfaces:**
- Consumes: `Identity` from `./contracts.js`.
- Produces: `extractPin(rawName: string): string | null`, `splitDisplayName(rawName: string, pin: string | null): { displayName: string; location: string }`, `identityFromName(rawName: string): Identity`.

**Divergence note (intentional, spec §3):** the patch's splitter branched on segment count *and* `hasPin`, which produced `location = " "` for `"Name | 1383"` and made the 4-segment case depend on PIN presence. The clean rule below — first segment is the name, the first later segment that is not the PIN is the location — reproduces every observed output without the branch tangle.

- [ ] **Step 1: Write the failing test**

`show-engine/src/identity.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { extractPin, identityFromName, splitDisplayName } from "./identity.js";

describe("extractPin", () => {
  it("finds a standalone 4-digit PIN", () => {
    expect(extractPin("Roy Meyers | 1383 | Forest Hill, MD")).toBe("1383");
  });

  it("returns the first PIN when several are present", () => {
    expect(extractPin("Ann 1383 Bee 4242")).toBe("1383");
  });

  it("returns null when there is no 4-digit token", () => {
    expect(extractPin("Roy Meyers | Forest Hill, MD")).toBeNull();
    expect(extractPin("Room 12345")).toBeNull();
    expect(extractPin("Desk 123")).toBeNull();
  });
});

describe("splitDisplayName", () => {
  it("takes the first segment as the name and the next non-PIN segment as location", () => {
    expect(splitDisplayName("Roy Meyers | dd02 | Forest Hill, MD | US", null)).toEqual({
      displayName: "Roy Meyers",
      location: "dd02"
    });
  });

  it("skips the PIN segment when choosing a location", () => {
    expect(splitDisplayName("J.J. Mc Kenna | 1383 | Santa Venetia, CA", "1383")).toEqual({
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA"
    });
  });

  it("yields an empty location when the PIN is the only other segment", () => {
    expect(splitDisplayName("J.J. Mc Kenna | 1383", "1383")).toEqual({
      displayName: "J.J. Mc Kenna",
      location: ""
    });
  });

  it("accepts a slash separator", () => {
    expect(splitDisplayName("Ann Lee / Austin, TX", null)).toEqual({
      displayName: "Ann Lee",
      location: "Austin, TX"
    });
  });

  it("yields an empty location for a bare name", () => {
    expect(splitDisplayName("Ann Lee", null)).toEqual({ displayName: "Ann Lee", location: "" });
  });

  it("ignores empty segments from doubled separators", () => {
    expect(splitDisplayName("Ann Lee ||  Austin, TX", null)).toEqual({
      displayName: "Ann Lee",
      location: "Austin, TX"
    });
  });
});

describe("identityFromName", () => {
  it("combines PIN extraction and name splitting", () => {
    expect(identityFromName("J.J. Mc Kenna | 1383 | Santa Venetia, CA")).toEqual({
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA",
      pin: "1383"
    });
  });

  it("handles an unregistered guest", () => {
    expect(identityFromName("Guest User")).toEqual({
      displayName: "Guest User",
      location: "",
      pin: null
    });
  });

  it("strips newlines from pasted names", () => {
    expect(identityFromName("Ann\nLee | Austin")).toEqual({
      displayName: "AnnLee",
      location: "Austin",
      pin: null
    });
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./identity.js`.

- [ ] **Step 3: Write the implementation**

`show-engine/src/identity.ts`:

```ts
/**
 * Display-name parsing: the OHG identity convention.
 * Panelists register in Mukana, receive a 4-digit PIN, and put it in their Zoom
 * display name (e.g. "J.J. Mc Kenna | 1383 | Santa Venetia, CA"). This module
 * recovers the PIN plus a best-effort display name and location, which is all
 * the identity an unregistered guest ever gets.
 */

import type { Identity } from "./contracts.js";

/** A standalone 4-digit token. Word boundaries keep "12345" and "123" from matching. */
const PIN_PATTERN = /\b\d{4}\b/;

/** Panelists separate name/location/PIN with either "|" or "/". */
const SEGMENT_SEPARATOR = /[/|]/;

/** Zoom permits pasted multi-line names; they break every downstream list. */
const NAME_NOISE = /[\n\r]+/g;

function sanitize(rawName: string): string {
  return rawName.replace(NAME_NOISE, "");
}

/** The first standalone 4-digit token in the name, or null when there is none. */
export function extractPin(rawName: string): string | null {
  const match = sanitize(rawName).match(PIN_PATTERN);
  return match === null ? null : match[0];
}

/**
 * Split a display name into name and location. The first segment is always the
 * name; the location is the first later segment that is not the PIN itself.
 */
export function splitDisplayName(
  rawName: string,
  pin: string | null
): { displayName: string; location: string } {
  const segments = sanitize(rawName)
    .split(SEGMENT_SEPARATOR)
    .map((segment) => segment.trim())
    .filter((segment) => segment.length > 0);

  const displayName = segments[0] ?? "";
  const location = segments.slice(1).find((segment) => segment !== pin) ?? "";
  return { displayName, location };
}

/** Full identity parse of a raw Zoom display name. */
export function identityFromName(rawName: string): Identity {
  const pin = extractPin(rawName);
  const { displayName, location } = splitDisplayName(rawName, pin);
  return { displayName, location, pin };
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/identity.ts show-engine/src/identity.test.ts
git commit -m "feat(show-engine): PIN extraction and display-name splitting"
```

---

### Task 4: Participant ingest cache

**Files:**
- Create: `show-engine/src/zoomIngest.ts`
- Test: `show-engine/src/zoomIngest.test.ts`

**Interfaces:**
- Consumes: `Participant` from `./contracts.js`.
- Produces: type `ZoomEvent` (union), class `ZoomIngest` with `apply(event: ZoomEvent): void`, `commit(): boolean`, `snapshot(): readonly Participant[]`, `get dirty(): boolean`.

**Behavior note:** the double buffer reproduces `Zoom_Cached_Data`'s working/published split — a bulk roster replacement must never be observable half-applied. `commit()` publishes and returns whether anything actually changed, so callers can skip downstream work.

- [ ] **Step 1: Write the failing test**

`show-engine/src/zoomIngest.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { ZoomIngest } from "./zoomIngest.js";
import type { Participant } from "./contracts.js";

function participant(overrides: Partial<Participant> & { participantId: string }): Participant {
  return {
    rawName: "Guest",
    online: true,
    videoOn: false,
    audioOn: false,
    handRaised: false,
    zoomRole: 3,
    ...overrides
  };
}

describe("ZoomIngest", () => {
  it("publishes nothing before the first commit", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    expect(ingest.snapshot()).toEqual([]);
    expect(ingest.dirty).toBe(true);
  });

  it("publishes the working set on commit", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    expect(ingest.commit()).toBe(true);
    expect(ingest.snapshot().map((p) => p.participantId)).toEqual(["a"]);
    expect(ingest.dirty).toBe(false);
  });

  it("reports no change when committing twice", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.commit();
    expect(ingest.commit()).toBe(false);
  });

  it("orders the snapshot by participant id for stable downstream diffing", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "c" }) });
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.apply({ kind: "joined", participant: participant({ participantId: "b" }) });
    ingest.commit();
    expect(ingest.snapshot().map((p) => p.participantId)).toEqual(["a", "b", "c"]);
  });

  it("replaces the whole roster on a roster event", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.commit();
    ingest.apply({
      kind: "roster",
      participants: [participant({ participantId: "b" }), participant({ participantId: "c" })]
    });
    ingest.commit();
    expect(ingest.snapshot().map((p) => p.participantId)).toEqual(["b", "c"]);
  });

  it("applies video, audio, hand and rename events", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.apply({ kind: "video", participantId: "a", on: true });
    ingest.apply({ kind: "audio", participantId: "a", on: true });
    ingest.apply({ kind: "hand", participantId: "a", raised: true });
    ingest.apply({ kind: "renamed", participantId: "a", rawName: "Ann | 1383" });
    ingest.commit();
    const [first] = ingest.snapshot();
    expect(first).toMatchObject({
      videoOn: true,
      audioOn: true,
      handRaised: true,
      rawName: "Ann | 1383"
    });
  });

  it("keeps a departed participant but marks them offline with video off", () => {
    const ingest = new ZoomIngest();
    ingest.apply({
      kind: "joined",
      participant: participant({ participantId: "a", videoOn: true })
    });
    ingest.apply({ kind: "left", participantId: "a" });
    ingest.commit();
    expect(ingest.snapshot()[0]).toMatchObject({ online: false, videoOn: false });
  });

  it("ignores events for unknown participants", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "video", participantId: "ghost", on: true });
    expect(ingest.commit()).toBe(false);
    expect(ingest.snapshot()).toEqual([]);
  });

  it("does not mark dirty when an event changes nothing", () => {
    const ingest = new ZoomIngest();
    ingest.apply({
      kind: "joined",
      participant: participant({ participantId: "a", videoOn: true })
    });
    ingest.commit();
    ingest.apply({ kind: "video", participantId: "a", on: true });
    expect(ingest.dirty).toBe(false);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./zoomIngest.js`.

- [ ] **Step 3: Write the implementation**

`show-engine/src/zoomIngest.ts`:

```ts
/**
 * Participant ingest cache.
 * Turns the host's discrete Zoom roster events into a stable published
 * snapshot. Replaces the patch's ZoomOSC listeners plus `Zoom_Cached_Data`:
 * the working/published double buffer means a bulk roster replacement is never
 * observed half-applied by downstream stages.
 */

import type { Participant } from "./contracts.js";

export type ZoomEvent =
  | { kind: "roster"; participants: Participant[] }
  | { kind: "joined"; participant: Participant }
  | { kind: "left"; participantId: string }
  | { kind: "video"; participantId: string; on: boolean }
  | { kind: "audio"; participantId: string; on: boolean }
  | { kind: "hand"; participantId: string; raised: boolean }
  | { kind: "renamed"; participantId: string; rawName: string };

export class ZoomIngest {
  private working = new Map<string, Participant>();
  private published: readonly Participant[] = [];
  private isDirty = false;

  get dirty(): boolean {
    return this.isDirty;
  }

  apply(event: ZoomEvent): void {
    switch (event.kind) {
      case "roster": {
        this.working = new Map(event.participants.map((p) => [p.participantId, { ...p }]));
        this.isDirty = true;
        return;
      }
      case "joined": {
        this.working.set(event.participant.participantId, { ...event.participant });
        this.isDirty = true;
        return;
      }
      case "left": {
        this.mutate(event.participantId, { online: false, videoOn: false });
        return;
      }
      case "video": {
        this.mutate(event.participantId, { videoOn: event.on });
        return;
      }
      case "audio": {
        this.mutate(event.participantId, { audioOn: event.on });
        return;
      }
      case "hand": {
        this.mutate(event.participantId, { handRaised: event.raised });
        return;
      }
      case "renamed": {
        this.mutate(event.participantId, { rawName: event.rawName });
        return;
      }
    }
  }

  /** Publish the working set. Returns whether the published snapshot changed. */
  commit(): boolean {
    if (!this.isDirty) return false;
    this.published = [...this.working.values()].sort((a, b) =>
      a.participantId.localeCompare(b.participantId)
    );
    this.isDirty = false;
    return true;
  }

  snapshot(): readonly Participant[] {
    return this.published;
  }

  private mutate(participantId: string, patch: Partial<Participant>): void {
    const current = this.working.get(participantId);
    if (current === undefined) return;

    const next = { ...current, ...patch };
    if (!changed(current, next)) return;

    this.working.set(participantId, next);
    this.isDirty = true;
  }
}

function changed(a: Participant, b: Participant): boolean {
  return (
    a.rawName !== b.rawName ||
    a.online !== b.online ||
    a.videoOn !== b.videoOn ||
    a.audioOn !== b.audioOn ||
    a.handRaised !== b.handRaised ||
    a.zoomRole !== b.zoomRole
  );
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/zoomIngest.ts show-engine/src/zoomIngest.test.ts
git commit -m "feat(show-engine): participant ingest cache with published snapshot"
```

---

### Task 5: Mukana response parsing and registry

**Files:**
- Create: `show-engine/src/mukanaParse.ts`
- Test: `show-engine/src/mukanaParse.test.ts`

**Interfaces:**
- Consumes: `MukanaDb`, `MukanaRecord`, `coerceRole` from `./contracts.js`.
- Produces: type `MukanaOutcome` (`{ kind: "data"; db: MukanaDb } | { kind: "dormant"; detail: string } | { kind: "invalid"; reason: string }`), `parseMukanaPanelists(body: string): MukanaOutcome`, class `MukanaRegistry` with `merge(db: MukanaDb): void`, `current(): MukanaDb`, `purge(): void`.

**Behavior note:** the live endpoint returns a `{"status":200,...,"detail":"This page is only available between 1300 and 2000 UTC"}` envelope outside show hours. That body must never be parsed as panelist data — it becomes a `dormant` outcome so the caller keeps its last-good registry.

- [ ] **Step 1: Write the failing test**

`show-engine/src/mukanaParse.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { MukanaRegistry, parseMukanaPanelists } from "./mukanaParse.js";

const panelistsBody = JSON.stringify({
  "0B6FTPaUEF": {
    displayName: "J.J. Mc Kenna",
    loc: "Santa Venetia, CA, US",
    pin: 1383,
    role: "host",
    online: true,
    uid: "0B6FTPaUEF"
  },
  ZZ9PluralZAlpha: {
    displayName: "Ann Lee",
    loc: "Austin, TX, US",
    pin: 4242,
    role: "panelist",
    online: false,
    uid: "ZZ9PluralZAlpha"
  }
});

describe("parseMukanaPanelists", () => {
  it("re-keys records by PIN", () => {
    const outcome = parseMukanaPanelists(panelistsBody);
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(Object.keys(outcome.db).sort()).toEqual(["1383", "4242"]);
    expect(outcome.db["1383"]).toEqual({
      pin: "1383",
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA, US",
      role: "host",
      online: true
    });
  });

  it("treats the off-hours status envelope as dormant", () => {
    const outcome = parseMukanaPanelists(
      JSON.stringify({
        status: 200,
        source: "/var/www/html/phpsdk/php-panel-rest.php",
        detail: "This page is only available between 1300 and 2000 UTC"
      })
    );
    expect(outcome.kind).toBe("dormant");
    if (outcome.kind !== "dormant") return;
    expect(outcome.detail).toMatch(/1300 and 2000 UTC/);
  });

  it("reports unparseable bodies as invalid", () => {
    const outcome = parseMukanaPanelists("<html>502 Bad Gateway</html>");
    expect(outcome.kind).toBe("invalid");
  });

  it("skips records without a PIN", () => {
    const outcome = parseMukanaPanelists(
      JSON.stringify({ uidA: { displayName: "No Pin", loc: "Nowhere", role: "panelist" } })
    );
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(Object.keys(outcome.db)).toEqual([]);
  });

  it("coerces an unknown role to panelist", () => {
    const outcome = parseMukanaPanelists(
      JSON.stringify({ uidA: { displayName: "Odd", loc: "X", pin: 1111, role: "moderator" } })
    );
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.db["1111"]?.role).toBe("panelist");
  });
});

describe("MukanaRegistry", () => {
  it("starts empty", () => {
    expect(new MukanaRegistry().current()).toEqual({});
  });

  it("merges successive fetches, with later data winning", () => {
    const registry = new MukanaRegistry();
    registry.merge({
      "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host", online: true }
    });
    registry.merge({
      "1383": { pin: "1383", displayName: "J.J. Mc Kenna", location: "CA", role: "host", online: false },
      "4242": { pin: "4242", displayName: "Ann Lee", location: "TX", role: "panelist", online: true }
    });
    const db = registry.current();
    expect(db["1383"]?.displayName).toBe("J.J. Mc Kenna");
    expect(db["1383"]?.online).toBe(false);
    expect(Object.keys(db).sort()).toEqual(["1383", "4242"]);
  });

  it("retains records absent from a later fetch", () => {
    const registry = new MukanaRegistry();
    registry.merge({
      "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host", online: true }
    });
    registry.merge({
      "4242": { pin: "4242", displayName: "Ann", location: "TX", role: "panelist", online: true }
    });
    expect(Object.keys(registry.current()).sort()).toEqual(["1383", "4242"]);
  });

  it("drops everything on purge", () => {
    const registry = new MukanaRegistry();
    registry.merge({
      "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host", online: true }
    });
    registry.purge();
    expect(registry.current()).toEqual({});
  });

  it("returns a copy so callers cannot mutate internal state", () => {
    const registry = new MukanaRegistry();
    registry.merge({
      "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host", online: true }
    });
    const db = registry.current();
    delete db["1383"];
    expect(Object.keys(registry.current())).toEqual(["1383"]);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./mukanaParse.js`.

- [ ] **Step 3: Write the implementation**

`show-engine/src/mukanaParse.ts`:

```ts
/**
 * Mukana registry parsing and accumulation.
 * The Mukana REST backend is the show's panelist registry: people register,
 * receive a 4-digit PIN, and the engine joins them to Zoom participants on it.
 * Records arrive keyed by Firebase UID and are re-keyed by PIN here.
 *
 * Outside show hours the endpoint returns a status envelope rather than data;
 * that is a `dormant` outcome, never an error and never parsed as panelists.
 */

import { coerceRole, type MukanaDb, type MukanaRecord } from "./contracts.js";

export type MukanaOutcome =
  | { kind: "data"; db: MukanaDb }
  | { kind: "dormant"; detail: string }
  | { kind: "invalid"; reason: string };

function readString(source: Record<string, unknown>, key: string): string {
  const value = source[key];
  return typeof value === "string" ? value.trim() : "";
}

function readPin(source: Record<string, unknown>): string | null {
  const value = source.pin;
  if (typeof value === "number" && Number.isInteger(value)) return String(value);
  if (typeof value === "string" && value.trim().length > 0) return value.trim();
  return null;
}

/** Parse a raw panelists response body into a PIN-keyed registry. */
export function parseMukanaPanelists(body: string): MukanaOutcome {
  let parsed: unknown;
  try {
    parsed = JSON.parse(body);
  } catch {
    return { kind: "invalid", reason: "response body is not JSON" };
  }

  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
    return { kind: "invalid", reason: "response body is not a JSON object" };
  }

  const root = parsed as Record<string, unknown>;
  if ("status" in root) {
    const detail = readString(root, "detail");
    return { kind: "dormant", detail: detail.length > 0 ? detail : "registry dormant" };
  }

  const db: MukanaDb = {};
  for (const value of Object.values(root)) {
    if (typeof value !== "object" || value === null || Array.isArray(value)) continue;
    const record = value as Record<string, unknown>;

    const pin = readPin(record);
    if (pin === null) continue;

    const entry: MukanaRecord = {
      pin,
      displayName: readString(record, "displayName"),
      location: readString(record, "loc"),
      role: coerceRole(record.role),
      online: record.online === true
    };
    db[pin] = entry;
  }

  return { kind: "data", db };
}

/**
 * Accumulates successive fetches. Records are never removed by a later fetch
 * that omits them — a panelist who drops out of the registry mid-show keeps
 * their identity until an explicit purge.
 */
export class MukanaRegistry {
  private db: MukanaDb = {};

  merge(incoming: MukanaDb): void {
    this.db = { ...this.db, ...incoming };
  }

  current(): MukanaDb {
    return { ...this.db };
  }

  purge(): void {
    this.db = {};
  }
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/mukanaParse.ts show-engine/src/mukanaParse.test.ts
git commit -m "feat(show-engine): Mukana response parsing with dormant-hours gate"
```

---

### Task 6: Mukana polling client

**Files:**
- Create: `show-engine/src/mukanaClient.ts`
- Test: `show-engine/src/mukanaClient.test.ts`

**Interfaces:**
- Consumes: `MukanaConfig` from `./config.js`; `parseMukanaPanelists`, `MukanaOutcome` from `./mukanaParse.js`.
- Produces: types `FetchResponse`, `FetchLike`, `MukanaHealth`; class `MukanaClient` with `constructor(config: MukanaConfig, deps: { fetch: FetchLike })`, `fetchPanelists(): Promise<MukanaOutcome>`, `get health(): MukanaHealth`, `nextDelayMs(): number`.

**Design note:** no timer lives here. The client performs one fetch per call and reports the delay the caller should wait; the orchestrator (Plan 2) owns the loop. That keeps this fully unit-testable with no fake clocks.

- [ ] **Step 1: Write the failing test**

`show-engine/src/mukanaClient.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { MukanaClient, type FetchLike } from "./mukanaClient.js";
import type { MukanaConfig } from "./config.js";

const config: MukanaConfig = {
  baseUrl: "https://hoka.example.com/php-panel-rest.php",
  event: "officehours",
  panelistsIntervalMs: 5000,
  handsIntervalMs: 2000,
  questionIntervalMs: 2000,
  maxBackoffMs: 60000
};

function respondWith(body: string, ok = true, status = 200): FetchLike {
  return async () => ({ ok, status, text: async () => body });
}

const panelistsBody = JSON.stringify({
  uidA: { displayName: "Ann Lee", loc: "Austin, TX", pin: 4242, role: "panelist", online: true }
});

describe("MukanaClient", () => {
  it("requests the panelists endpoint with the configured event", async () => {
    const urls: string[] = [];
    const client = new MukanaClient(config, {
      fetch: async (url) => {
        urls.push(url);
        return { ok: true, status: 200, text: async () => panelistsBody };
      }
    });
    await client.fetchPanelists();
    expect(urls).toEqual([
      "https://hoka.example.com/php-panel-rest.php?event=officehours&req=panelists"
    ]);
  });

  it("reports healthy after a successful fetch", async () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("data");
    expect(client.health).toEqual({ state: "ok", consecutiveFailures: 0, detail: null });
    expect(client.nextDelayMs()).toBe(5000);
  });

  it("reports dormant without counting a failure", async () => {
    const client = new MukanaClient(config, {
      fetch: respondWith(JSON.stringify({ status: 200, detail: "outside show hours" }))
    });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("dormant");
    expect(client.health).toEqual({
      state: "dormant",
      consecutiveFailures: 0,
      detail: "outside show hours"
    });
    expect(client.nextDelayMs()).toBe(5000);
  });

  it("turns a thrown network error into an invalid outcome", async () => {
    const client = new MukanaClient(config, {
      fetch: async () => {
        throw new Error("ECONNREFUSED");
      }
    });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("invalid");
    expect(client.health.state).toBe("failing");
    expect(client.health.detail).toMatch(/ECONNREFUSED/);
  });

  it("treats a non-2xx response as a failure", async () => {
    const client = new MukanaClient(config, { fetch: respondWith("nope", false, 503) });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("invalid");
    expect(client.health.detail).toMatch(/503/);
  });

  it("backs off exponentially and caps at maxBackoffMs", async () => {
    const client = new MukanaClient(config, { fetch: respondWith("nope", false, 503) });
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(10000);
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(20000);
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(40000);
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(60000);
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(60000);
  });

  it("resets backoff after a recovery", async () => {
    let body = "nope";
    let ok = false;
    const client = new MukanaClient(config, {
      fetch: async () => ({ ok, status: ok ? 200 : 503, text: async () => body })
    });
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(10000);

    body = panelistsBody;
    ok = true;
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(5000);
    expect(client.health.consecutiveFailures).toBe(0);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./mukanaClient.js`.

- [ ] **Step 3: Write the implementation**

`show-engine/src/mukanaClient.ts`:

```ts
/**
 * Mukana REST client.
 * Performs a single fetch per call and reports how long the caller should wait
 * before the next one — the polling loop lives in the orchestrator, which keeps
 * this unit-testable without fake timers. Network failures back off
 * exponentially; a dormant registry is not a failure and does not back off.
 */

import type { MukanaConfig } from "./config.js";
import { parseMukanaPanelists, type MukanaOutcome } from "./mukanaParse.js";

export type FetchResponse = {
  ok: boolean;
  status: number;
  text: () => Promise<string>;
};

export type FetchLike = (url: string) => Promise<FetchResponse>;

export type MukanaHealth = {
  state: "ok" | "dormant" | "failing";
  consecutiveFailures: number;
  detail: string | null;
};

export class MukanaClient {
  private readonly config: MukanaConfig;
  private readonly fetch: FetchLike;
  private state: MukanaHealth = { state: "ok", consecutiveFailures: 0, detail: null };

  constructor(config: MukanaConfig, deps: { fetch: FetchLike }) {
    this.config = config;
    this.fetch = deps.fetch;
  }

  get health(): MukanaHealth {
    return { ...this.state };
  }

  /** Milliseconds to wait before the next panelists fetch. */
  nextDelayMs(): number {
    const { consecutiveFailures } = this.state;
    if (consecutiveFailures === 0) return this.config.panelistsIntervalMs;

    const backoff = this.config.panelistsIntervalMs * 2 ** consecutiveFailures;
    return Math.min(backoff, this.config.maxBackoffMs);
  }

  async fetchPanelists(): Promise<MukanaOutcome> {
    return this.request("panelists");
  }

  private async request(req: string): Promise<MukanaOutcome> {
    const url = `${this.config.baseUrl}?event=${encodeURIComponent(this.config.event)}&req=${req}`;

    let body: string;
    try {
      const response = await this.fetch(url);
      if (!response.ok) {
        return this.fail(`HTTP ${response.status} from ${req}`);
      }
      body = await response.text();
    } catch (error) {
      const detail = error instanceof Error ? error.message : String(error);
      return this.fail(detail);
    }

    const outcome = parseMukanaPanelists(body);
    if (outcome.kind === "invalid") {
      return this.fail(outcome.reason);
    }

    if (outcome.kind === "dormant") {
      this.state = { state: "dormant", consecutiveFailures: 0, detail: outcome.detail };
      return outcome;
    }

    this.state = { state: "ok", consecutiveFailures: 0, detail: null };
    return outcome;
  }

  private fail(detail: string): MukanaOutcome {
    this.state = {
      state: "failing",
      consecutiveFailures: this.state.consecutiveFailures + 1,
      detail
    };
    return { kind: "invalid", reason: detail };
  }
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/mukanaClient.ts show-engine/src/mukanaClient.test.ts
git commit -m "feat(show-engine): Mukana polling client with backoff and health"
```

---

### Task 7: Operator role overrides

**Files:**
- Create: `show-engine/src/overrideDb.ts`
- Test: `show-engine/src/overrideDb.test.ts`

**Interfaces:**
- Consumes: `MukanaDb`, `Role` from `./contracts.js`.
- Produces: type `OverrideRecord` (`{ pin: string; displayName: string; location: string; role: Role }`), class `OverrideDb` with `set(record: OverrideRecord): void`, `delete(pin: string): void`, `assignExclusiveRole(pin: string, role: "host" | "reader", mukana: MukanaDb): void`, `entries(): Record<string, OverrideRecord>`, `roleFor(pin: string): Role | undefined`, `clear(): void`, `restore(entries: Record<string, OverrideRecord>): void`.

**Algorithm (from `Javascript__93`, cleaned):** assigning an exclusive role must guarantee exactly one holder across *both* the override table and the Mukana registry:
1. Every Mukana record already declaring that role, not yet overridden, gets an override demoting it to `panelist`.
2. Every existing override holding that role is demoted to `panelist` if Mukana also declares it, and otherwise deleted outright (the override was the only thing granting it).
3. The target PIN's override row is written with the role.

- [ ] **Step 1: Write the failing test**

`show-engine/src/overrideDb.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { OverrideDb } from "./overrideDb.js";
import type { MukanaDb } from "./contracts.js";

const mukana: MukanaDb = {
  "1383": {
    pin: "1383",
    displayName: "J.J. Mc Kenna",
    location: "Santa Venetia, CA",
    role: "host",
    online: true
  },
  "4242": {
    pin: "4242",
    displayName: "Ann Lee",
    location: "Austin, TX",
    role: "panelist",
    online: true
  },
  "5555": {
    pin: "5555",
    displayName: "Bo Diaz",
    location: "Lima, PE",
    role: "panelist",
    online: true
  }
};

describe("OverrideDb", () => {
  it("starts empty", () => {
    expect(new OverrideDb().entries()).toEqual({});
  });

  it("stores and reads back an override", () => {
    const db = new OverrideDb();
    db.set({ pin: "4242", displayName: "Ann Lee", location: "Austin, TX", role: "aslinterpreter" });
    expect(db.roleFor("4242")).toBe("aslinterpreter");
  });

  it("deletes an override", () => {
    const db = new OverrideDb();
    db.set({ pin: "4242", displayName: "Ann Lee", location: "Austin, TX", role: "reader" });
    db.delete("4242");
    expect(db.roleFor("4242")).toBeUndefined();
  });

  it("demotes a Mukana-declared host when another PIN is promoted", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("4242", "host", mukana);
    expect(db.roleFor("4242")).toBe("host");
    expect(db.roleFor("1383")).toBe("panelist");
  });

  it("carries the demoted person's identity into the override row", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("4242", "host", mukana);
    expect(db.entries()["1383"]).toEqual({
      pin: "1383",
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA",
      role: "panelist"
    });
  });

  it("deletes a previous override-only host rather than leaving a demotion row", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("5555", "host", mukana);
    expect(db.roleFor("5555")).toBe("host");

    db.assignExclusiveRole("4242", "host", mukana);
    expect(db.roleFor("4242")).toBe("host");
    expect(db.entries()["5555"]).toBeUndefined();
  });

  it("leaves the reader alone when assigning a host", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("5555", "reader", mukana);
    db.assignExclusiveRole("4242", "host", mukana);
    expect(db.roleFor("5555")).toBe("reader");
    expect(db.roleFor("4242")).toBe("host");
  });

  it("yields exactly one holder of an exclusive role after repeated assignment", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("4242", "host", mukana);
    db.assignExclusiveRole("5555", "host", mukana);
    db.assignExclusiveRole("1383", "host", mukana);
    const hosts = Object.values(db.entries()).filter((entry) => entry.role === "host");
    expect(hosts.map((entry) => entry.pin)).toEqual(["1383"]);
  });

  it("uses the Mukana identity when promoting an unknown-to-overrides PIN", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("4242", "reader", mukana);
    expect(db.entries()["4242"]).toEqual({
      pin: "4242",
      displayName: "Ann Lee",
      location: "Austin, TX",
      role: "reader"
    });
  });

  it("promotes a PIN absent from Mukana with empty identity fields", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("7777", "host", mukana);
    expect(db.entries()["7777"]).toEqual({
      pin: "7777",
      displayName: "",
      location: "",
      role: "host"
    });
  });

  it("restores a persisted table and clears on demand", () => {
    const db = new OverrideDb();
    db.restore({
      "4242": { pin: "4242", displayName: "Ann Lee", location: "Austin, TX", role: "host" }
    });
    expect(db.roleFor("4242")).toBe("host");
    db.clear();
    expect(db.entries()).toEqual({});
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./overrideDb.js`.

- [ ] **Step 3: Write the implementation**

`show-engine/src/overrideDb.ts`:

```ts
/**
 * Operator role overrides.
 * Mukana declares each panelist's role, but the operator must be able to move
 * the host or reader chair mid-show. This table takes precedence over Mukana
 * in the panelist join, and it guarantees the exclusive roles have exactly one
 * holder across both sources.
 */

import type { MukanaDb, Role } from "./contracts.js";

export type OverrideRecord = {
  pin: string;
  displayName: string;
  location: string;
  role: Role;
};

export class OverrideDb {
  private entriesByPin = new Map<string, OverrideRecord>();

  set(record: OverrideRecord): void {
    this.entriesByPin.set(record.pin, { ...record });
  }

  delete(pin: string): void {
    this.entriesByPin.delete(pin);
  }

  roleFor(pin: string): Role | undefined {
    return this.entriesByPin.get(pin)?.role;
  }

  entries(): Record<string, OverrideRecord> {
    return Object.fromEntries(
      [...this.entriesByPin.entries()].map(([pin, record]) => [pin, { ...record }])
    );
  }

  clear(): void {
    this.entriesByPin.clear();
  }

  restore(entries: Record<string, OverrideRecord>): void {
    this.entriesByPin = new Map(
      Object.entries(entries).map(([pin, record]) => [pin, { ...record }])
    );
  }

  /**
   * Give `pin` an exclusive role, guaranteeing it is the only holder.
   * Prior holders declared by Mukana are demoted with an explicit override row;
   * prior holders that existed only as overrides have their row removed.
   */
  assignExclusiveRole(pin: string, role: "host" | "reader", mukana: MukanaDb): void {
    for (const record of Object.values(mukana)) {
      if (record.role !== role) continue;
      if (this.entriesByPin.has(record.pin)) continue;
      this.entriesByPin.set(record.pin, {
        pin: record.pin,
        displayName: record.displayName,
        location: record.location,
        role: "panelist"
      });
    }

    for (const [existingPin, record] of [...this.entriesByPin.entries()]) {
      if (record.role !== role) continue;
      if (mukana[existingPin]?.role === role) {
        this.entriesByPin.set(existingPin, { ...record, role: "panelist" });
      } else {
        this.entriesByPin.delete(existingPin);
      }
    }

    const mukanaRecord = mukana[pin];
    this.entriesByPin.set(pin, {
      pin,
      displayName: mukanaRecord?.displayName ?? "",
      location: mukanaRecord?.location ?? "",
      role
    });
  }
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/overrideDb.ts show-engine/src/overrideDb.test.ts
git commit -m "feat(show-engine): operator role overrides with exclusive-role guarantee"
```

---

### Task 8: Panelist database join

**Files:**
- Create: `show-engine/src/panelistDb.ts`
- Test: `show-engine/src/panelistDb.test.ts`

**Interfaces:**
- Consumes: `Participant`, `Panelist`, `MukanaDb` from `./contracts.js`; `identityFromName` from `./identity.js`; `OverrideRecord` from `./overrideDb.js`.
- Produces: `buildPanelistDb(participants: readonly Participant[], mukana: MukanaDb, overrides: Record<string, OverrideRecord>): Map<string, Panelist>`.

**Precedence:** override → Mukana → name-parsed fallback, field by field. `role` defaults to `"panelist"`. `hasMukana` reflects a real registry hit only.

- [ ] **Step 1: Write the failing test**

`show-engine/src/panelistDb.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { buildPanelistDb } from "./panelistDb.js";
import type { MukanaDb, Participant } from "./contracts.js";

function participant(participantId: string, rawName: string): Participant {
  return {
    participantId,
    rawName,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3
  };
}

const mukana: MukanaDb = {
  "1383": {
    pin: "1383",
    displayName: "J.J. Mc Kenna",
    location: "Santa Venetia, CA, US",
    role: "host",
    online: true
  }
};

describe("buildPanelistDb", () => {
  it("uses Mukana identity and role when the PIN matches", () => {
    const db = buildPanelistDb([participant("p1", "JJ | 1383 | somewhere")], mukana, {});
    const panelist = db.get("p1");
    expect(panelist).toMatchObject({
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA, US",
      pin: "1383",
      role: "host",
      hasMukana: true
    });
  });

  it("falls back to the parsed display name when there is no Mukana record", () => {
    const db = buildPanelistDb([participant("p2", "Guest User | Austin, TX")], mukana, {});
    expect(db.get("p2")).toMatchObject({
      displayName: "Guest User",
      location: "Austin, TX",
      pin: null,
      role: "panelist",
      hasMukana: false
    });
  });

  it("treats an unregistered PIN as no Mukana record", () => {
    const db = buildPanelistDb([participant("p3", "Ann Lee | 9999 | Austin")], mukana, {});
    expect(db.get("p3")).toMatchObject({ pin: "9999", hasMukana: false, role: "panelist" });
  });

  it("lets an override win over Mukana", () => {
    const db = buildPanelistDb([participant("p1", "JJ | 1383")], mukana, {
      "1383": {
        pin: "1383",
        displayName: "JJ (stand-in)",
        location: "Remote",
        role: "panelist"
      }
    });
    expect(db.get("p1")).toMatchObject({
      displayName: "JJ (stand-in)",
      location: "Remote",
      role: "panelist",
      hasMukana: true
    });
  });

  it("keeps Mukana identity when the override carries only a role", () => {
    const db = buildPanelistDb([participant("p1", "JJ | 1383")], mukana, {
      "1383": { pin: "1383", displayName: "", location: "", role: "reader" }
    });
    expect(db.get("p1")).toMatchObject({
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA, US",
      role: "reader"
    });
  });

  it("preserves participant liveness fields", () => {
    const db = buildPanelistDb(
      [{ ...participant("p1", "JJ | 1383"), videoOn: false, handRaised: true, online: false }],
      mukana,
      {}
    );
    expect(db.get("p1")).toMatchObject({ videoOn: false, handRaised: true, online: false });
  });

  it("keys the database by participant id", () => {
    const db = buildPanelistDb(
      [participant("p1", "JJ | 1383"), participant("p2", "Ann Lee")],
      mukana,
      {}
    );
    expect([...db.keys()].sort()).toEqual(["p1", "p2"]);
  });

  it("returns an empty map for an empty roster", () => {
    expect(buildPanelistDb([], mukana, {}).size).toBe(0);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./panelistDb.js`.

- [ ] **Step 3: Write the implementation**

`show-engine/src/panelistDb.ts`:

```ts
/**
 * The panelist join — the show's single source of truth for identity.
 * Joins live Zoom participants against the Mukana registry and the operator
 * override table on the 4-digit PIN. Precedence is override, then Mukana, then
 * the name parsed out of the Zoom display name, field by field, so an override
 * that carries only a role change never blanks a person's name.
 */

import type { MukanaDb, Panelist, Participant } from "./contracts.js";
import { identityFromName } from "./identity.js";
import type { OverrideRecord } from "./overrideDb.js";

function pick(...candidates: (string | undefined)[]): string {
  for (const candidate of candidates) {
    if (candidate !== undefined && candidate.length > 0) return candidate;
  }
  return "";
}

/** Build the master panelist database, keyed by participant id. */
export function buildPanelistDb(
  participants: readonly Participant[],
  mukana: MukanaDb,
  overrides: Record<string, OverrideRecord>
): Map<string, Panelist> {
  const db = new Map<string, Panelist>();

  for (const participant of participants) {
    const identity = identityFromName(participant.rawName);
    const mukanaRecord = identity.pin === null ? undefined : mukana[identity.pin];
    const override = identity.pin === null ? undefined : overrides[identity.pin];

    db.set(participant.participantId, {
      ...participant,
      pin: identity.pin,
      displayName: pick(override?.displayName, mukanaRecord?.displayName, identity.displayName),
      location: pick(override?.location, mukanaRecord?.location, identity.location),
      role: override?.role ?? mukanaRecord?.role ?? "panelist",
      hasMukana: mukanaRecord !== undefined
    });
  }

  return db;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/panelistDb.ts show-engine/src/panelistDb.test.ts
git commit -m "feat(show-engine): panelist database join over PIN"
```

---

### Task 9: Live slots — core mechanics

**Files:**
- Create: `show-engine/src/liveSlots.ts`
- Test: `show-engine/src/liveSlots.test.ts`

**Interfaces:**
- Consumes: `Panelist`, `Slot` from `./contracts.js`.
- Produces: type `LiveSlotsOptions` (`{ capacity: number; utilityPinBase: number }`), class `LiveSlots` with `constructor(options: LiveSlotsOptions)`, `slots(): Slot[]`, `add(panelist: Panelist): number | null`, `removeSlot(slot: number): void`, `replace(slot: number, panelist: Panelist): void`, `slotOf(participantId: string): number | null`, `occupiedCount(): number`.

Task 10 extends this same class with utility-tail placement, role uniqueness, `refresh`, `rebuild`, and JSON round-tripping. Write this task's code so those additions slot in cleanly (keep placement logic in a private `placementFor` method).

**Behavior:** a fixed array of `capacity` slots. `add` takes the first empty slot. `removeSlot` leaves a hole — it never compacts, because an operator's slot layout is a deliberate arrangement. Adding a participant who already holds a slot is a no-op that returns their existing slot.

- [ ] **Step 1: Write the failing test**

`show-engine/src/liveSlots.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { LiveSlots } from "./liveSlots.js";
import type { Panelist } from "./contracts.js";

function panelist(participantId: string, overrides: Partial<Panelist> = {}): Panelist {
  return {
    participantId,
    rawName: `Name ${participantId}`,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3,
    displayName: `Name ${participantId}`,
    location: "",
    pin: null,
    hasMukana: false,
    role: "panelist",
    ...overrides
  };
}

function makeSlots(capacity = 4): LiveSlots {
  return new LiveSlots({ capacity, utilityPinBase: 9000 });
}

describe("LiveSlots core mechanics", () => {
  it("starts with the configured number of empty slots", () => {
    const slots = makeSlots();
    expect(slots.slots()).toEqual([
      { slot: 1, panelist: null },
      { slot: 2, panelist: null },
      { slot: 3, panelist: null },
      { slot: 4, panelist: null }
    ]);
    expect(slots.occupiedCount()).toBe(0);
  });

  it("adds panelists into ascending slots", () => {
    const slots = makeSlots();
    expect(slots.add(panelist("a"))).toBe(1);
    expect(slots.add(panelist("b"))).toBe(2);
    expect(slots.occupiedCount()).toBe(2);
  });

  it("returns the existing slot when adding a panelist twice", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    expect(slots.add(panelist("a"))).toBe(1);
    expect(slots.occupiedCount()).toBe(1);
  });

  it("returns null when every slot is taken", () => {
    const slots = makeSlots(2);
    slots.add(panelist("a"));
    slots.add(panelist("b"));
    expect(slots.add(panelist("c"))).toBeNull();
  });

  it("leaves a hole on removal instead of compacting", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    slots.add(panelist("b"));
    slots.add(panelist("c"));
    slots.removeSlot(2);
    expect(slots.slots().map((entry) => entry.panelist?.participantId ?? null)).toEqual([
      "a",
      null,
      "c",
      null
    ]);
  });

  it("fills the first hole on the next add", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    slots.add(panelist("b"));
    slots.add(panelist("c"));
    slots.removeSlot(2);
    expect(slots.add(panelist("d"))).toBe(2);
  });

  it("replaces the occupant of a slot in place", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    slots.replace(1, panelist("z"));
    expect(slots.slotOf("z")).toBe(1);
    expect(slots.slotOf("a")).toBeNull();
  });

  it("clears any prior slot when replacing with a panelist already seated", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    slots.add(panelist("b"));
    slots.replace(1, panelist("b"));
    expect(slots.slotOf("b")).toBe(1);
    expect(slots.slots()[1]).toEqual({ slot: 2, panelist: null });
  });

  it("reports null for an unseated participant", () => {
    expect(makeSlots().slotOf("nobody")).toBeNull();
  });

  it("rejects out-of-range slot numbers", () => {
    const slots = makeSlots();
    expect(() => slots.removeSlot(0)).toThrow(/slot/);
    expect(() => slots.removeSlot(5)).toThrow(/slot/);
    expect(() => slots.replace(9, panelist("a"))).toThrow(/slot/);
  });

  it("rejects a capacity below 1", () => {
    expect(() => new LiveSlots({ capacity: 0, utilityPinBase: 9000 })).toThrow(/capacity/);
  });

  it("returns copies so callers cannot mutate internal state", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    const view = slots.slots();
    view[0] = { slot: 1, panelist: null };
    expect(slots.slotOf("a")).toBe(1);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./liveSlots.js`.

- [ ] **Step 3: Write the implementation**

`show-engine/src/liveSlots.ts`:

```ts
/**
 * The live slot roster — the heart of the show engine.
 * A fixed array of `capacity` positions, each mapping to one video slot the
 * host can deliver. People keep their slot until an operator moves them, and
 * removal leaves a hole rather than compacting, because the arrangement on
 * screen is a deliberate editorial choice.
 */

import type { Panelist, Slot } from "./contracts.js";

export type LiveSlotsOptions = {
  capacity: number;
  utilityPinBase: number;
};

export class LiveSlots {
  private readonly capacity: number;
  private readonly utilityPinBase: number;
  private readonly seats: (Panelist | null)[];

  constructor(options: LiveSlotsOptions) {
    if (!Number.isInteger(options.capacity) || options.capacity < 1) {
      throw new Error(`LiveSlots capacity must be an integer >= 1, got ${options.capacity}`);
    }
    this.capacity = options.capacity;
    this.utilityPinBase = options.utilityPinBase;
    this.seats = new Array<Panelist | null>(options.capacity).fill(null);
  }

  slots(): Slot[] {
    return this.seats.map((panelist, index) => ({
      slot: index + 1,
      panelist: panelist === null ? null : { ...panelist }
    }));
  }

  occupiedCount(): number {
    return this.seats.reduce((count, seat) => (seat === null ? count : count + 1), 0);
  }

  slotOf(participantId: string): number | null {
    const index = this.seats.findIndex((seat) => seat?.participantId === participantId);
    return index === -1 ? null : index + 1;
  }

  /** Seat a panelist. Returns the slot taken, their existing slot, or null when full. */
  add(panelist: Panelist): number | null {
    const existing = this.slotOf(panelist.participantId);
    if (existing !== null) return existing;

    const slot = this.placementFor(panelist);
    if (slot === null) return null;

    this.seats[slot - 1] = { ...panelist };
    return slot;
  }

  removeSlot(slot: number): void {
    this.assertSlot(slot);
    this.seats[slot - 1] = null;
  }

  replace(slot: number, panelist: Panelist): void {
    this.assertSlot(slot);
    const previous = this.slotOf(panelist.participantId);
    if (previous !== null && previous !== slot) {
      this.seats[previous - 1] = null;
    }
    this.seats[slot - 1] = { ...panelist };
  }

  /**
   * Choose a slot for a newcomer. Task 10 extends this with the utility-PIN
   * tail rule; the base behavior is the first empty slot.
   */
  protected placementFor(_panelist: Panelist): number | null {
    return this.firstEmptySlot();
  }

  protected firstEmptySlot(): number | null {
    const index = this.seats.findIndex((seat) => seat === null);
    return index === -1 ? null : index + 1;
  }

  protected assertSlot(slot: number): void {
    if (!Number.isInteger(slot) || slot < 1 || slot > this.capacity) {
      throw new Error(`slot ${slot} is out of range 1..${this.capacity}`);
    }
  }
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/liveSlots.ts show-engine/src/liveSlots.test.ts
git commit -m "feat(show-engine): live slot roster with holes and stable seating"
```

---

### Task 10: Live slots — utility tail, role uniqueness, refresh, persistence shape

**Files:**
- Modify: `show-engine/src/liveSlots.ts`
- Test: `show-engine/src/liveSlots.test.ts` (append a new `describe` block; keep Task 9's tests unchanged)

**Interfaces:**
- Consumes: everything from Task 9, plus `EXCLUSIVE_ROLES` from `./contracts.js`.
- Produces: added to `LiveSlots` — `rebuild(panelists: readonly Panelist[]): void`, `refresh(db: Map<string, Panelist>): void`, `toJSON(): LiveSlotsState`, `static fromJSON(state: LiveSlotsState, options: LiveSlotsOptions): LiveSlots`; exported type `LiveSlotsState` (`{ version: 1; capacity: number; seats: ({ slot: number; panelist: Panelist } | null)[] }`).

**Three behaviors:**
1. **Utility tail.** A panelist whose PIN is numerically `>= utilityPinBase` is a graphics bot or playback machine, not a person: it seats from the *end*, at `capacity - (pin - utilityPinBase)`. If that exact slot is taken it scans downward toward slot 1; if the whole tail is full it falls back to the first empty slot.
2. **Exclusive-role uniqueness.** Seating or replacing with a host/reader demotes any other seated holder of that role to `"panelist"` in the roster view. (The authoritative assignment lives in `OverrideDb`; this keeps the on-screen roster self-consistent even mid-change.)
3. **Refresh in place.** Re-pull each seated person from the master DB by participant id: present → refresh their fields, slot unchanged; absent → keep the seat but mark `online: false, videoOn: false`, so a vanished participant is visibly offline rather than silently dropped.

- [ ] **Step 1: Write the failing test**

Append to `show-engine/src/liveSlots.test.ts`:

```ts
describe("LiveSlots utility tail", () => {
  it("seats a utility PIN at the last slot", () => {
    const slots = makeSlots(8);
    expect(slots.add(panelist("bot", { pin: "9000" }))).toBe(8);
  });

  it("offsets successive utility PINs from the end", () => {
    const slots = makeSlots(8);
    expect(slots.add(panelist("bot0", { pin: "9000" }))).toBe(8);
    expect(slots.add(panelist("bot1", { pin: "9001" }))).toBe(7);
    expect(slots.add(panelist("bot2", { pin: "9002" }))).toBe(6);
  });

  it("keeps people out of the tail slots taken by bots", () => {
    const slots = makeSlots(4);
    slots.add(panelist("bot", { pin: "9000" }));
    expect(slots.add(panelist("person"))).toBe(1);
  });

  it("scans downward when the target tail slot is taken", () => {
    const slots = makeSlots(4);
    slots.add(panelist("bot0", { pin: "9000" }));
    expect(slots.add(panelist("bot0b", { pin: "9000" }))).toBe(3);
  });

  it("falls back to the first empty slot when the tail is exhausted", () => {
    const slots = makeSlots(2);
    slots.add(panelist("bot0", { pin: "9000" }));
    slots.add(panelist("bot1", { pin: "9001" }));
    slots.removeSlot(1);
    expect(slots.add(panelist("bot2", { pin: "9002" }))).toBe(1);
  });

  it("treats a non-numeric PIN as an ordinary panelist", () => {
    const slots = makeSlots(4);
    expect(slots.add(panelist("odd", { pin: "abcd" }))).toBe(1);
  });
});

describe("LiveSlots exclusive roles", () => {
  it("demotes a prior host when a new host is seated", () => {
    const slots = makeSlots();
    slots.add(panelist("a", { role: "host" }));
    slots.add(panelist("b", { role: "host" }));
    expect(slots.slots()[0]?.panelist?.role).toBe("panelist");
    expect(slots.slots()[1]?.panelist?.role).toBe("host");
  });

  it("demotes a prior reader on replace", () => {
    const slots = makeSlots();
    slots.add(panelist("a", { role: "reader" }));
    slots.add(panelist("b"));
    slots.replace(2, panelist("c", { role: "reader" }));
    expect(slots.slots()[0]?.panelist?.role).toBe("panelist");
    expect(slots.slots()[1]?.panelist?.role).toBe("reader");
  });

  it("leaves the host alone when a reader is seated", () => {
    const slots = makeSlots();
    slots.add(panelist("a", { role: "host" }));
    slots.add(panelist("b", { role: "reader" }));
    expect(slots.slots()[0]?.panelist?.role).toBe("host");
  });

  it("does not restrict non-exclusive roles", () => {
    const slots = makeSlots();
    slots.add(panelist("a", { role: "aslinterpreter" }));
    slots.add(panelist("b", { role: "aslinterpreter" }));
    expect(slots.slots().map((entry) => entry.panelist?.role)).toEqual([
      "aslinterpreter",
      "aslinterpreter",
      undefined,
      undefined
    ]);
  });
});

describe("LiveSlots rebuild and refresh", () => {
  it("rebuilds from a roster, seating bots in the tail", () => {
    const slots = makeSlots(4);
    slots.rebuild([
      panelist("a"),
      panelist("bot", { pin: "9000" }),
      panelist("b")
    ]);
    expect(slots.slots().map((entry) => entry.panelist?.participantId ?? null)).toEqual([
      "a",
      "b",
      null,
      "bot"
    ]);
  });

  it("clears previous occupants on rebuild", () => {
    const slots = makeSlots(4);
    slots.add(panelist("old"));
    slots.rebuild([panelist("new")]);
    expect(slots.slotOf("old")).toBeNull();
    expect(slots.slotOf("new")).toBe(1);
  });

  it("refreshes seated panelists in place without moving them", () => {
    const slots = makeSlots(4);
    slots.add(panelist("a"));
    slots.add(panelist("b"));
    slots.refresh(
      new Map([
        ["a", panelist("a", { displayName: "Renamed A", videoOn: false })],
        ["b", panelist("b")]
      ])
    );
    expect(slots.slotOf("a")).toBe(1);
    expect(slots.slots()[0]?.panelist).toMatchObject({
      displayName: "Renamed A",
      videoOn: false
    });
  });

  it("marks a vanished participant offline but keeps their seat", () => {
    const slots = makeSlots(4);
    slots.add(panelist("a"));
    slots.refresh(new Map());
    expect(slots.slotOf("a")).toBe(1);
    expect(slots.slots()[0]?.panelist).toMatchObject({ online: false, videoOn: false });
  });

  it("re-applies role uniqueness on refresh", () => {
    const slots = makeSlots(4);
    slots.add(panelist("a", { role: "host" }));
    slots.add(panelist("b"));
    slots.refresh(
      new Map([
        ["a", panelist("a", { role: "host" })],
        ["b", panelist("b", { role: "host" })]
      ])
    );
    const roles = slots.slots().map((entry) => entry.panelist?.role);
    expect(roles.filter((role) => role === "host")).toHaveLength(1);
  });
});

describe("LiveSlots persistence", () => {
  it("round-trips through JSON", () => {
    const slots = makeSlots(4);
    slots.add(panelist("a", { role: "host" }));
    slots.add(panelist("bot", { pin: "9000" }));
    slots.removeSlot(1);
    slots.add(panelist("c"));

    const restored = LiveSlots.fromJSON(slots.toJSON(), {
      capacity: 4,
      utilityPinBase: 9000
    });
    expect(restored.slots()).toEqual(slots.slots());
  });

  it("rejects a state whose capacity disagrees with the options", () => {
    const slots = makeSlots(4);
    expect(() =>
      LiveSlots.fromJSON(slots.toJSON(), { capacity: 8, utilityPinBase: 9000 })
    ).toThrow(/capacity/);
  });
});
```

- [ ] **Step 2: Run the tests to verify the new block fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `slots.rebuild is not a function`, and utility-tail expectations fail (`add` returns 1, not 8).

- [ ] **Step 3: Extend the implementation**

In `show-engine/src/liveSlots.ts`, change the import line to also pull in `EXCLUSIVE_ROLES`:

```ts
import { EXCLUSIVE_ROLES, type Panelist, type Slot } from "./contracts.js";
```

Add the state type below `LiveSlotsOptions`:

```ts
export type LiveSlotsState = {
  version: 1;
  capacity: number;
  seats: ({ slot: number; panelist: Panelist } | null)[];
};
```

Replace the `add`, `replace`, and `placementFor` members, and append the new methods, so the class body reads:

```ts
  /** Seat a panelist. Returns the slot taken, their existing slot, or null when full. */
  add(panelist: Panelist): number | null {
    const existing = this.slotOf(panelist.participantId);
    if (existing !== null) return existing;

    const slot = this.placementFor(panelist);
    if (slot === null) return null;

    this.seats[slot - 1] = { ...panelist };
    this.enforceExclusiveRole(slot);
    return slot;
  }

  replace(slot: number, panelist: Panelist): void {
    this.assertSlot(slot);
    const previous = this.slotOf(panelist.participantId);
    if (previous !== null && previous !== slot) {
      this.seats[previous - 1] = null;
    }
    this.seats[slot - 1] = { ...panelist };
    this.enforceExclusiveRole(slot);
  }

  /** Clear every seat and re-seat the given roster in order. */
  rebuild(panelists: readonly Panelist[]): void {
    this.seats.fill(null);
    for (const panelist of panelists) {
      this.add(panelist);
    }
  }

  /**
   * Re-pull every seated panelist from the master database. Seats never move.
   * A participant who has vanished from the database keeps their seat but is
   * marked offline — visibly gone rather than silently dropped.
   */
  refresh(db: Map<string, Panelist>): void {
    this.seats.forEach((seat, index) => {
      if (seat === null) return;
      const fresh = db.get(seat.participantId);
      this.seats[index] =
        fresh === undefined ? { ...seat, online: false, videoOn: false } : { ...fresh };
    });

    this.seats.forEach((seat, index) => {
      if (seat !== null && isExclusive(seat.role)) this.enforceExclusiveRole(index + 1);
    });
  }

  toJSON(): LiveSlotsState {
    return {
      version: 1,
      capacity: this.capacity,
      seats: this.seats.map((panelist, index) =>
        panelist === null ? null : { slot: index + 1, panelist: { ...panelist } }
      )
    };
  }

  static fromJSON(state: LiveSlotsState, options: LiveSlotsOptions): LiveSlots {
    if (state.capacity !== options.capacity) {
      throw new Error(
        `persisted capacity ${state.capacity} does not match configured capacity ${options.capacity}`
      );
    }
    const restored = new LiveSlots(options);
    state.seats.forEach((seat, index) => {
      if (seat !== null) restored.seats[index] = { ...seat.panelist };
    });
    return restored;
  }

  /**
   * Utility participants (graphics bots, playback machines) carry a PIN at or
   * above `utilityPinBase` and seat from the end, keeping the low slots free
   * for people. `pin - utilityPinBase` is the offset from the last slot.
   */
  protected placementFor(panelist: Panelist): number | null {
    const utilitySlot = this.utilitySlotFor(panelist);
    if (utilitySlot !== null) return utilitySlot;
    return this.firstEmptySlot();
  }

  private utilitySlotFor(panelist: Panelist): number | null {
    if (panelist.pin === null) return null;
    const pin = Number(panelist.pin);
    if (!Number.isInteger(pin) || pin < this.utilityPinBase) return null;

    const target = this.capacity - (pin - this.utilityPinBase);
    for (let slot = Math.min(target, this.capacity); slot >= 1; slot -= 1) {
      if (this.seats[slot - 1] === null) return slot;
    }
    return null;
  }

  /** Demote every other seated holder of this seat's exclusive role. */
  private enforceExclusiveRole(slot: number): void {
    const seated = this.seats[slot - 1];
    if (seated === undefined || seated === null || !isExclusive(seated.role)) return;

    this.seats.forEach((other, index) => {
      if (other === null || index === slot - 1) return;
      if (other.role === seated.role) {
        this.seats[index] = { ...other, role: "panelist" };
      }
    });
  }
```

Add this helper at the end of the file, outside the class:

```ts
function isExclusive(role: Panelist["role"]): boolean {
  return (EXCLUSIVE_ROLES as readonly string[]).includes(role);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine`
Expected: PASS — Task 9's block still green plus the four new blocks.

- [ ] **Step 5: Verify types compile**

Run: `npm run typecheck --workspace show-engine`
Expected: no output (success).

- [ ] **Step 6: Commit**

```bash
git add show-engine/src/liveSlots.ts show-engine/src/liveSlots.test.ts
git commit -m "feat(show-engine): utility tail, role uniqueness, refresh and slot persistence"
```

---

### Task 11: Atomic state persistence

**Files:**
- Create: `show-engine/src/persistence.ts`
- Test: `show-engine/src/persistence.test.ts`

**Interfaces:**
- Consumes: `LiveSlotsState` from `./liveSlots.js`; `OverrideRecord` from `./overrideDb.js`.
- Produces: type `ShowState` (`{ version: 1; slots: LiveSlotsState; overrides: Record<string, OverrideRecord> }`), type `StateFs` (`{ readFile, writeFile, rename, mkdir }`), class `StateStore` with `constructor(path: string, deps: { fs: StateFs })`, `save(state: ShowState): Promise<void>`, `load(): Promise<ShowState | null>`.

**Why atomic:** the show state file is rewritten on every roster change during a live daily show. A partial write from a crash mid-save would take the next restart down. Write to `<path>.tmp`, then rename — rename is atomic on both APFS and NTFS.

- [ ] **Step 1: Write the failing test**

`show-engine/src/persistence.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { StateStore, type ShowState, type StateFs } from "./persistence.js";

const state: ShowState = {
  version: 1,
  slots: { version: 1, capacity: 2, seats: [null, null] },
  overrides: {
    "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host" }
  }
};

function fakeFs(seed: Record<string, string> = {}): StateFs & { files: Map<string, string> } {
  const files = new Map<string, string>(Object.entries(seed));
  return {
    files,
    async readFile(path) {
      const content = files.get(path);
      if (content === undefined) {
        const error: NodeJS.ErrnoException = new Error(`ENOENT: ${path}`);
        error.code = "ENOENT";
        throw error;
      }
      return content;
    },
    async writeFile(path, content) {
      files.set(path, content);
    },
    async rename(from, to) {
      const content = files.get(from);
      if (content === undefined) throw new Error(`ENOENT: ${from}`);
      files.delete(from);
      files.set(to, content);
    },
    async mkdir() {
      /* directories are implicit in the fake */
    }
  };
}

describe("StateStore", () => {
  it("writes to a temp file then renames into place", async () => {
    const fs = fakeFs();
    const order: string[] = [];
    const store = new StateStore("/show/state.json", {
      fs: {
        ...fs,
        async writeFile(path, content) {
          order.push(`write:${path}`);
          await fs.writeFile(path, content);
        },
        async rename(from, to) {
          order.push(`rename:${from}->${to}`);
          await fs.rename(from, to);
        }
      }
    });

    await store.save(state);
    expect(order).toEqual([
      "write:/show/state.json.tmp",
      "rename:/show/state.json.tmp->/show/state.json"
    ]);
    expect(fs.files.has("/show/state.json.tmp")).toBe(false);
  });

  it("round-trips a saved state", async () => {
    const fs = fakeFs();
    const store = new StateStore("/show/state.json", { fs });
    await store.save(state);
    expect(await store.load()).toEqual(state);
  });

  it("returns null when no state file exists", async () => {
    const store = new StateStore("/show/state.json", { fs: fakeFs() });
    expect(await store.load()).toBeNull();
  });

  it("returns null for a corrupt state file rather than throwing", async () => {
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({ "/show/state.json": "{ truncated" })
    });
    expect(await store.load()).toBeNull();
  });

  it("returns null for a state file of an unknown version", async () => {
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({ "/show/state.json": JSON.stringify({ ...state, version: 99 }) })
    });
    expect(await store.load()).toBeNull();
  });

  it("creates the parent directory before writing", async () => {
    const made: string[] = [];
    const fs = fakeFs();
    const store = new StateStore("/show/nested/state.json", {
      fs: {
        ...fs,
        async mkdir(path) {
          made.push(path);
        }
      }
    });
    await store.save(state);
    expect(made).toEqual(["/show/nested"]);
  });

  it("propagates a write failure loudly", async () => {
    const fs = fakeFs();
    const store = new StateStore("/show/state.json", {
      fs: {
        ...fs,
        async writeFile() {
          throw new Error("ENOSPC: no space left on device");
        }
      }
    });
    await expect(store.save(state)).rejects.toThrow(/ENOSPC/);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./persistence.js`.

- [ ] **Step 3: Write the implementation**

`show-engine/src/persistence.ts`:

```ts
/**
 * Show-state persistence.
 * The slot roster and override table are rewritten on every roster change
 * during a live show, so saves are atomic: write a temp file, then rename over
 * the target. A crash mid-save leaves the previous good state intact.
 *
 * Loads are forgiving by design — a missing or unreadable state file yields
 * null so the engine starts clean, rather than refusing to boot before a show.
 */

import type { LiveSlotsState } from "./liveSlots.js";
import type { OverrideRecord } from "./overrideDb.js";

export type ShowState = {
  version: 1;
  slots: LiveSlotsState;
  overrides: Record<string, OverrideRecord>;
};

export type StateFs = {
  readFile: (path: string) => Promise<string>;
  writeFile: (path: string, content: string) => Promise<void>;
  rename: (from: string, to: string) => Promise<void>;
  mkdir: (path: string) => Promise<void>;
};

const STATE_VERSION = 1;

function parentDirectory(path: string): string {
  const index = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return index <= 0 ? path : path.slice(0, index);
}

export class StateStore {
  private readonly path: string;
  private readonly fs: StateFs;

  constructor(path: string, deps: { fs: StateFs }) {
    this.path = path;
    this.fs = deps.fs;
  }

  async save(state: ShowState): Promise<void> {
    const tempPath = `${this.path}.tmp`;
    await this.fs.mkdir(parentDirectory(this.path));
    await this.fs.writeFile(tempPath, JSON.stringify(state, null, 2));
    await this.fs.rename(tempPath, this.path);
  }

  /** Read persisted state, or null when it is absent, corrupt, or a foreign version. */
  async load(): Promise<ShowState | null> {
    let content: string;
    try {
      content = await this.fs.readFile(this.path);
    } catch {
      return null;
    }

    let parsed: unknown;
    try {
      parsed = JSON.parse(content);
    } catch {
      return null;
    }

    if (typeof parsed !== "object" || parsed === null) return null;
    const candidate = parsed as Partial<ShowState>;
    if (candidate.version !== STATE_VERSION) return null;
    if (candidate.slots === undefined || candidate.overrides === undefined) return null;

    return candidate as ShowState;
  }
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/persistence.ts show-engine/src/persistence.test.ts
git commit -m "feat(show-engine): atomic show-state persistence"
```

---

### Task 12: Public exports and pipeline integration test

**Files:**
- Create: `show-engine/src/index.ts`
- Test: `show-engine/src/pipeline.test.ts`

**Interfaces:**
- Consumes: every module built in Tasks 1–11.
- Produces: the package's public surface. Plan 2 imports from `@corevideo/show-engine` (or directly from `./src/*.js` within the package).

This task proves the modules compose: participant events plus a Mukana fetch produce a correctly seated, persistable roster — the plan's deliverable.

- [ ] **Step 1: Write the failing integration test**

`show-engine/src/pipeline.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import {
  buildPanelistDb,
  LiveSlots,
  MukanaClient,
  MukanaRegistry,
  OverrideDb,
  parseShowEngineConfig,
  StateStore,
  ZoomIngest,
  type FetchLike,
  type Participant,
  type ShowState,
  type StateFs
} from "./index.js";

const config = parseShowEngineConfig({
  capacity: 4,
  statePath: "/show/state.json",
  mukana: { baseUrl: "https://hoka.example.com/rest.php", event: "officehours" }
});

const mukanaBody = JSON.stringify({
  uidHost: {
    displayName: "J.J. Mc Kenna",
    loc: "Santa Venetia, CA",
    pin: 1383,
    role: "host",
    online: true
  },
  uidPanelist: {
    displayName: "Ann Lee",
    loc: "Austin, TX",
    pin: 4242,
    role: "panelist",
    online: true
  }
});

const fetchMukana: FetchLike = async () => ({
  ok: true,
  status: 200,
  text: async () => mukanaBody
});

function participant(participantId: string, rawName: string): Participant {
  return {
    participantId,
    rawName,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3
  };
}

function memoryFs(): StateFs {
  const files = new Map<string, string>();
  return {
    async readFile(path) {
      const content = files.get(path);
      if (content === undefined) throw new Error(`ENOENT: ${path}`);
      return content;
    },
    async writeFile(path, content) {
      files.set(path, content);
    },
    async rename(from, to) {
      const content = files.get(from);
      if (content === undefined) throw new Error(`ENOENT: ${from}`);
      files.delete(from);
      files.set(to, content);
    },
    async mkdir() {
      /* no-op */
    }
  };
}

describe("identity and roster pipeline", () => {
  it("seats registered panelists with their Mukana identity and role", async () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z1", "JJ | 1383 | somewhere") });
    ingest.apply({ kind: "joined", participant: participant("z2", "Ann | 4242 | Austin") });
    ingest.apply({ kind: "joined", participant: participant("z3", "Walk-in Guest") });
    ingest.commit();

    const registry = new MukanaRegistry();
    const client = new MukanaClient(config.mukana, { fetch: fetchMukana });
    const outcome = await client.fetchPanelists();
    if (outcome.kind === "data") registry.merge(outcome.db);

    const overrides = new OverrideDb();
    const db = buildPanelistDb(ingest.snapshot(), registry.current(), overrides.entries());

    const slots = new LiveSlots({
      capacity: config.capacity,
      utilityPinBase: config.utilityPinBase
    });
    for (const panelist of db.values()) slots.add(panelist);

    const seated = slots.slots().map((entry) =>
      entry.panelist === null
        ? null
        : { name: entry.panelist.displayName, role: entry.panelist.role }
    );
    expect(seated).toEqual([
      { name: "J.J. Mc Kenna", role: "host" },
      { name: "Ann Lee", role: "panelist" },
      { name: "Walk-in Guest", role: "panelist" },
      null
    ]);
  });

  it("moves the host chair via an override without moving anyone's slot", async () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z1", "JJ | 1383") });
    ingest.apply({ kind: "joined", participant: participant("z2", "Ann | 4242") });
    ingest.commit();

    const registry = new MukanaRegistry();
    const client = new MukanaClient(config.mukana, { fetch: fetchMukana });
    const outcome = await client.fetchPanelists();
    if (outcome.kind === "data") registry.merge(outcome.db);

    const overrides = new OverrideDb();
    const slots = new LiveSlots({
      capacity: config.capacity,
      utilityPinBase: config.utilityPinBase
    });
    for (const panelist of buildPanelistDb(
      ingest.snapshot(),
      registry.current(),
      overrides.entries()
    ).values()) {
      slots.add(panelist);
    }
    expect(slots.slotOf("z1")).toBe(1);

    overrides.assignExclusiveRole("4242", "host", registry.current());
    slots.refresh(buildPanelistDb(ingest.snapshot(), registry.current(), overrides.entries()));

    expect(slots.slotOf("z1")).toBe(1);
    expect(slots.slotOf("z2")).toBe(2);
    expect(slots.slots()[0]?.panelist?.role).toBe("panelist");
    expect(slots.slots()[1]?.panelist?.role).toBe("host");
  });

  it("keeps a reconnecting panelist's slot across a persistence round trip", async () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z1", "JJ | 1383") });
    ingest.apply({ kind: "joined", participant: participant("z2", "Ann | 4242") });
    ingest.commit();

    const registry = new MukanaRegistry();
    const client = new MukanaClient(config.mukana, { fetch: fetchMukana });
    const outcome = await client.fetchPanelists();
    if (outcome.kind === "data") registry.merge(outcome.db);

    const overrides = new OverrideDb();
    const slots = new LiveSlots({
      capacity: config.capacity,
      utilityPinBase: config.utilityPinBase
    });
    slots.rebuild([
      ...buildPanelistDb(ingest.snapshot(), registry.current(), overrides.entries()).values()
    ]);

    const store = new StateStore(config.statePath, { fs: memoryFs() });
    const saved: ShowState = {
      version: 1,
      slots: slots.toJSON(),
      overrides: overrides.entries()
    };
    await store.save(saved);

    const loaded = await store.load();
    expect(loaded).not.toBeNull();
    if (loaded === null) return;

    const restored = LiveSlots.fromJSON(loaded.slots, {
      capacity: config.capacity,
      utilityPinBase: config.utilityPinBase
    });
    expect(restored.slotOf("z2")).toBe(2);

    ingest.apply({ kind: "left", participantId: "z2" });
    ingest.commit();
    restored.refresh(
      buildPanelistDb(ingest.snapshot(), registry.current(), overrides.entries())
    );
    expect(restored.slotOf("z2")).toBe(2);
    expect(restored.slots()[1]?.panelist?.online).toBe(false);
  });

  it("keeps the last-good registry when Mukana goes dormant", async () => {
    const registry = new MukanaRegistry();
    let body = mukanaBody;
    const client = new MukanaClient(config.mukana, {
      fetch: async () => ({ ok: true, status: 200, text: async () => body })
    });

    const first = await client.fetchPanelists();
    if (first.kind === "data") registry.merge(first.db);
    expect(Object.keys(registry.current()).sort()).toEqual(["1383", "4242"]);

    body = JSON.stringify({ status: 200, detail: "This page is only available between 1300 and 2000 UTC" });
    const second = await client.fetchPanelists();
    expect(second.kind).toBe("dormant");
    if (second.kind === "data") registry.merge(second.db);

    expect(Object.keys(registry.current()).sort()).toEqual(["1383", "4242"]);
    expect(client.health.state).toBe("dormant");
  });

  it("seats a utility bot in the tail while people fill from the front", async () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z1", "JJ | 1383") });
    ingest.apply({ kind: "joined", participant: participant("bot", "Playback | 9000") });
    ingest.commit();

    const registry = new MukanaRegistry();
    const client = new MukanaClient(config.mukana, { fetch: fetchMukana });
    const outcome = await client.fetchPanelists();
    if (outcome.kind === "data") registry.merge(outcome.db);

    const slots = new LiveSlots({
      capacity: config.capacity,
      utilityPinBase: config.utilityPinBase
    });
    slots.rebuild([
      ...buildPanelistDb(ingest.snapshot(), registry.current(), new OverrideDb().entries()).values()
    ]);

    expect(slots.slotOf("z1")).toBe(1);
    expect(slots.slotOf("bot")).toBe(4);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./index.js`.

- [ ] **Step 3: Write the public export barrel**

`show-engine/src/index.ts`:

```ts
/**
 * Public surface of the OHG show engine.
 * Plan 1 ships the identity and roster core; direction and output modules
 * (speaker recency, gallery, looks, program bus, tally, graphics) arrive in
 * Plan 2 and are exported from here as they land.
 */

export {
  coerceRole,
  EXCLUSIVE_ROLES,
  isRole,
  ROLES,
  type Identity,
  type MukanaDb,
  type MukanaRecord,
  type Panelist,
  type Participant,
  type Role,
  type Slot
} from "./contracts.js";

export { parseShowEngineConfig, type MukanaConfig, type ShowEngineConfig } from "./config.js";
export { extractPin, identityFromName, splitDisplayName } from "./identity.js";
export { ZoomIngest, type ZoomEvent } from "./zoomIngest.js";
export { MukanaRegistry, parseMukanaPanelists, type MukanaOutcome } from "./mukanaParse.js";
export {
  MukanaClient,
  type FetchLike,
  type FetchResponse,
  type MukanaHealth
} from "./mukanaClient.js";
export { OverrideDb, type OverrideRecord } from "./overrideDb.js";
export { buildPanelistDb } from "./panelistDb.js";
export { LiveSlots, type LiveSlotsOptions, type LiveSlotsState } from "./liveSlots.js";
export { StateStore, type ShowState, type StateFs } from "./persistence.js";
```

- [ ] **Step 4: Run the full suite and typecheck**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS, no type errors. Fix any export-name mismatches surfaced here — this is the task that proves the module boundaries agree.

- [ ] **Step 5: Verify the package builds**

Run: `npm run build --workspace show-engine`
Expected: `dist/` is emitted with `.d.ts` files, no errors.

- [ ] **Step 6: Commit**

```bash
git add show-engine/src/index.ts show-engine/src/pipeline.test.ts
git commit -m "feat(show-engine): public exports and roster pipeline integration tests"
```

---

## Definition of Done

- [ ] `npm run test --workspace show-engine` passes.
- [ ] `npm run typecheck --workspace show-engine` is clean.
- [ ] `npm run build --workspace show-engine` emits `dist/` with declarations.
- [ ] The package is registered in the root `workspaces` array with a `test:show-engine` script.
- [ ] Every module carries its header comment and has an adjacent test file.
- [ ] All work is committed on `spec/ohg-show-engine`.

## What Plan 2 picks up

`speakerRecency`, `galleryDirector`, `handsQueue`, `lookDirector`, `programBus`, `tallyPublisher`, `gfxDirector`, the `HostAdapter` interface plus a mock adapter, and the orchestrator that owns the polling loop and debounced persistence. Plan 2 extends `ShowEngineConfig` with `spx`, `tally`, `looks`, and `skipRoles`, and extends `ShowState` with gallery state.
