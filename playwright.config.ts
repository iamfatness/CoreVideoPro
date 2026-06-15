import { defineConfig } from "playwright/test";

const rendererPort = process.env.COREVIDEO_E2E_PORT ?? "5173";
const rendererUrl = `http://127.0.0.1:${rendererPort}`;

export default defineConfig({
  testDir: "./tests/e2e",
  timeout: 60_000,
  globalSetup: "./tests/e2e/global-setup.mjs",
  webServer: {
    command: `npm run dev -- --host 127.0.0.1 --port ${rendererPort} --strictPort`,
    url: rendererUrl,
    timeout: 120_000,
    reuseExistingServer: !process.env.CI
  },
  use: {}
});