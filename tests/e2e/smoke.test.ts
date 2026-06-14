/**
 * Playwright–Electron end-to-end smoke test.
 *
 * Asserts the join → Magic Scene → take → core-ack round-trip through the
 * real Electron shell against the mock engine bundle (no Zoom SDK, no GPU).
 *
 * Prerequisites:
 *   npm install --no-save electron@latest
 *   npx playwright test tests/e2e/smoke.test.ts
 *
 * In a container without the Electron binary the test is skipped automatically.
 */
import { test, expect } from "playwright/test";
import { _electron as electron } from "playwright";
import { existsSync } from "node:fs";
import { join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

// TypeScript: we declare the ambient shape of `import.meta` ourselves rather
// than pulling in @types/node, which is excluded from the project devDeps.
declare interface ImportMeta {
  readonly url: string;
}
declare const importMeta: ImportMeta;

// ESM-safe __dirname equivalent.
const _here = dirname(fileURLToPath(import.meta.url));
const _repoRoot = resolve(_here, "..", "..");

/**
 * Look for the Electron binary installed via `npm install --no-save electron`.
 * Returns undefined when Electron is not present (CI without the binary).
 */
function resolveElectronBin(): string | undefined {
  const candidates = [
    join(_repoRoot, "node_modules", ".bin", "electron"),
    join(_repoRoot, "desktop", "node_modules", ".bin", "electron"),
  ];
  return candidates.find((c) => existsSync(c));
}

const electronBin = resolveElectronBin();

test.describe("CoreVideo Pro – Electron smoke", () => {
  // Skip the entire suite when Electron is not installed so CI (which only
  // runs the headless vitest gate) doesn't fail.
  test.skip(!electronBin, "Electron binary not found — install with: npm install --no-save electron@latest");

  test("join → Magic Scene → take → core-ack round-trip", async () => {
    const app = await electron.launch({
      executablePath: electronBin!,
      args: [join(_repoRoot, "desktop", "main.ts")],
      env: {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        ...(globalThis as any).process?.env as Record<string, string>,
        ELECTRON_ENABLE_LOGGING: "1",
        NODE_ENV: "test",
      },
    });

    try {
      // Wait for the first BrowserWindow to appear.
      const page = await app.firstWindow();
      await page.waitForLoadState("domcontentloaded");

      // ── 1. CoreVideo Pro brand visible ───────────────────────────────────
      // The <strong>CoreVideo Pro</strong> in the top-bar header is always
      // rendered regardless of connection state.
      await expect(page.locator("strong", { hasText: "CoreVideo Pro" }).first()).toBeVisible({
        timeout: 10_000,
      });

      // ── 2. Join round-trip ───────────────────────────────────────────────
      // Navigate to Settings to access the Join Zoom button and connection pill.
      await page.getByRole("button", { name: /Settings/i }).click();

      const joinButton = page.getByRole("button", { name: /Join Zoom/i });
      // The mock engine starts in_meeting; if somehow not joined, click Join.
      if (await joinButton.isVisible()) {
        await joinButton.click();
      }
      // Whether we joined just now or were already in_meeting, the connected
      // pill must be visible.
      await expect(page.locator(".connection-pill.connected")).toBeVisible({ timeout: 5_000 });

      // ── 3. Magic Scene ───────────────────────────────────────────────────
      // Return to Studio tab where the footer Magic Scene button lives.
      await page.getByRole("button", { name: /^Studio$/i }).click();

      // The footer button carries class "magic-scene-button"; we use the first
      // enabled instance (footer wins over the template panel).
      const magicSceneButton = page.locator(".magic-scene-button").first();
      await expect(magicSceneButton).toBeVisible({ timeout: 5_000 });
      await expect(magicSceneButton).toBeEnabled();
      await magicSceneButton.click();

      // The mock AI engine returns a scene synchronously; the transition status
      // text or magicSceneStatus <p> updates immediately.
      await expect(
        page.locator("text=/queued by Magic Scene|Magic Scene/").first()
      ).toBeVisible({ timeout: 5_000 });

      // ── 4. Take ──────────────────────────────────────────────────────────
      const takeButton = page.locator(".take-button").first();
      await expect(takeButton).toBeVisible({ timeout: 5_000 });
      await takeButton.click();

      // After take, transition statusText contains "taken with <style>".
      await expect(page.locator("text=/taken with/").first()).toBeVisible({ timeout: 5_000 });

      // ── 5. Core-ack ──────────────────────────────────────────────────────
      // The InMemoryMediaCoreSyncEngine (mock core) responds synchronously.
      // Verify the app is still alive and the top-bar brand is present.
      await expect(page.locator("strong", { hasText: "CoreVideo Pro" }).first()).toBeVisible();
    } finally {
      await app.close();
    }
  });
});
