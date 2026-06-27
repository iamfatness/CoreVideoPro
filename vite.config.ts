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
      "native-core/**",
      "tests/e2e/**",
      "src/App.test.tsx"
    ],
    testTimeout: 15_000,
    pool: "forks"
  }
});
