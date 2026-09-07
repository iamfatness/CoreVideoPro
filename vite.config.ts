import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  // Relative "./" is required so the WinUI shell can load the built SPA from the
  // filesystem (file://); absolute "/assets/…" paths resolve to the filesystem
  // root and produce a blank window.
  base: "./",
  plugins: [react()],
  server: {
    port: 5173,
    strictPort: false
  },
  test: {
    name: "unit",
    environment: "jsdom",
    globals: true,
    setupFiles: "./src/test/setup.ts",
    exclude: [
      "**/node_modules/**",
      "**/dist/**",
      // Git worktrees live under .claude/ and contain a FULL copy of the repo,
      // tests included — but no node_modules. Without this, vitest collects
      // every worktree's copy of every test and runs the suite once per
      // worktree: 1235 files / 17k tests locally against 26 real files, with
      // the native-core specs failing because they resolve
      // node_modules/tsx/dist/cli.mjs against their own workspace root. CI
      // never sees it (no worktrees there), so it looks like a local-only
      // mystery. Note "native-core/**" below is anchored at the repo root and
      // does NOT match .claude/worktrees/<name>/native-core/**.
      "**/.claude/**",
      "native-core/**",
      // Infrastructure tests use node:test and run in their own CI command.
      "scripts/tests/release-evidence.test.mjs",
      "scripts/tests/av-content-analysis.test.mjs",
      "scripts/tests/av-content-decode.test.mjs",
      "scripts/tests/frame-performance.test.mjs",
      "scripts/tests/program-buffer-smoke.test.mjs",
      "tests/e2e/**",
      "src/App.test.tsx"
    ],
    testTimeout: 15_000,
    pool: "forks"
  }
});
