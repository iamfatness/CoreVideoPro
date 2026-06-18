/**
 * Playwright–Electron end-to-end smoke test.
 *
 * Asserts the join → Magic Scene → take → core-ack round-trip through the
 * real Electron shell against the Node media-core stub (no Zoom SDK, no GPU).
 *
 * Prerequisites:
 *   npm install --no-save electron@latest tsx esbuild
 *   npm run test:e2e
 */
import { test, expect } from "playwright/test";
import { _electron as electron } from "playwright";
import { existsSync } from "node:fs";
import { join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const _here = dirname(fileURLToPath(import.meta.url));
const _repoRoot = resolve(_here, "..", "..");
const _rendererUrl = process.env.COREVIDEO_RENDERER_URL ?? "http://127.0.0.1:5173";
const _mainE2e =
  process.env.COREVIDEO_E2E_MAIN ?? join(_repoRoot, "desktop", "main-e2e.mjs");

function resolveElectronInstalled(): boolean {
  const roots = [join(_repoRoot, "node_modules"), join(_repoRoot, "desktop", "node_modules")];
  for (const root of roots) {
    const binary = join(root, "electron", "dist", process.platform === "win32" ? "electron.exe" : "electron");
    if (existsSync(binary)) {
      return true;
    }
  }
  return false;
}

const hasElectron = resolveElectronInstalled();

test.describe("CoreVideo Pro – Electron smoke", () => {
  test.skip(!hasElectron, "Electron binary not found — install with: npm install --no-save electron@latest");
  test.skip(!existsSync(_mainE2e), `E2E main bundle missing at ${_mainE2e} — run npm run test:e2e`);

  test("join → Magic Scene → take → core-ack round-trip", async () => {
    const app = await electron.launch({
      args: [_mainE2e],
      cwd: _repoRoot,
      env: {
        ...process.env,
        COREVIDEO_E2E: "1",
        COREVIDEO_RENDERER_URL: _rendererUrl,
        ELECTRON_ENABLE_LOGGING: "1",
        NODE_ENV: "test"
      }
    });

    try {
      const page = await app.firstWindow({ timeout: 45_000 });
      await page.waitForLoadState("domcontentloaded");

      await expect(page.locator("strong", { hasText: "CoreVideo Pro" }).first()).toBeVisible({
        timeout: 15_000
      });

      await page.getByRole("button", { name: /Settings/i }).click();

      const joinButton = page.getByRole("button", { name: /Join Zoom/i });
      if (await joinButton.isVisible()) {
        await joinButton.click();
      }
      await expect(page.locator(".connection-pill.connected")).toBeVisible({ timeout: 10_000 });

      await page.getByRole("button", { name: /^Studio$/i }).click();

      const magicSceneButton = page.locator(".magic-scene-button").first();
      await expect(magicSceneButton).toBeVisible({ timeout: 10_000 });
      await expect(magicSceneButton).toBeEnabled();
      await magicSceneButton.click();

      await expect(page.locator(".transition-status", { hasText: /queued by Magic Scene/i })).toBeVisible({
        timeout: 10_000
      });

      const takeButton = page.locator(".take-button").first();
      await expect(takeButton).toBeVisible({ timeout: 10_000 });
      await takeButton.click();

      await expect(page.getByRole("button", { name: /Interview.*Program/i })).toBeVisible({
        timeout: 10_000
      });

      await page.getByRole("button", { name: /Settings/i }).click();
      await expect(
        page.locator('[aria-label="Native core sync"] strong', { hasText: "live" }).first()
      ).toBeVisible({ timeout: 10_000 });
    } finally {
      await app.close();
    }
  });
});
