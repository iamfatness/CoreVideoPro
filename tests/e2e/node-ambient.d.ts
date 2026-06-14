/**
 * Minimal ambient declarations for the Node built-ins used in the Playwright
 * e2e tests.  @types/node is excluded from the project devDeps to keep the
 * CI footprint small; we declare only the shapes we actually need.
 */

declare module "node:fs" {
  export function existsSync(path: string): boolean;
}

declare module "node:path" {
  export function join(...paths: string[]): string;
  export function dirname(p: string): string;
  export function resolve(...paths: string[]): string;
}

declare module "node:url" {
  export function fileURLToPath(url: string | URL): string;
}

// Playwright sets import.meta.url in its test runner.
interface ImportMeta {
  readonly url: string;
}
