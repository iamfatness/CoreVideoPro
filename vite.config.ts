import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  // Relative "./" is required for Electron loadFile (file://); absolute "/assets/…"
  // paths resolve to the filesystem root and produce a blank window.
  // The web demo deploy sets VITE_DEMO_BASE=/pro/demo/ so built asset URLs are
  // emitted under that path for hosting at corevideo.iamfatness.us/pro/demo/.
  base: process.env.VITE_DEMO_BASE ?? "./",
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
