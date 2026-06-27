import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

const integrationFiles = [
  "src/App.test.tsx"
];

export default defineConfig({
  base: "/",
  plugins: [react()],
  test: {
    name: "integration",
    environment: "jsdom",
    globals: true,
    setupFiles: "./src/test/setup.ts",
    include: integrationFiles,
    exclude: ["**/node_modules/**", "**/dist/**"],
    testTimeout: 30_000,
    hookTimeout: 30_000,
    pool: "forks",
    fileParallelism: false,
    maxWorkers: 1
  }
});
