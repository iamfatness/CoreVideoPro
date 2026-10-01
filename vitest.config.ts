import { defineConfig } from "vitest/config";

// Root unit run: the packages that have tests but no CI job of their own.
// show-engine is an npm workspace with its own config (`npm run test:show-engine`);
// scripts/ tests use node:test (`npm run test:scripts`).
export default defineConfig({
  test: {
    name: "unit",
    environment: "node",
    include: [
      "services/**/*.test.mjs",
      "companion-module-corevideopro/src/**/*.test.ts"
    ],
    testTimeout: 15_000,
    pool: "forks"
  }
});
