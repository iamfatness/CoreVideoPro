import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  // Default "/" keeps local dev and the desktop/Electron build unchanged.
  // The web demo deploy sets VITE_DEMO_BASE=/pro/demo/ so built asset URLs are
  // emitted under that path for hosting at corevideo.iamfatness.us/pro/demo/.
  base: process.env.VITE_DEMO_BASE ?? "/",
  plugins: [react()],
  server: {
    port: 5173,
    strictPort: false
  },
  test: {
    environment: "jsdom",
    globals: true,
    setupFiles: "./src/test/setup.ts",
    exclude: ["**/node_modules/**", "**/dist/**", "native-core/**", "tests/e2e/**"],
    testTimeout: 15000
  }
});
