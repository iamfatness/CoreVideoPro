/**
 * Minimal ambient declarations for the Node + Electron surfaces the desktop
 * shell uses. They let `tsc -p desktop/tsconfig.json` statically check this
 * directory without installing `@types/node` or `electron` (neither is needed
 * for the stubs-only CI gate). At runtime, Node and Electron provide the real
 * implementations. Intentionally narrow — only what this code touches.
 */

interface ImportMeta {
  url: string;
}

declare const window: {
  coreVideoNative?: import("../../src/engine/nativeHostBridge").NativeHostBridge;
};

declare const process: {
  execPath: string;
  platform: string;
  argv: string[];
  env: Record<string, string | undefined>;
  exit(code?: number): never;
  stdin: NodeJS_ReadableStream;
  stdout: NodeJS_WritableStream;
};

declare const console: {
  log(...args: unknown[]): void;
  info(...args: unknown[]): void;
  warn(...args: unknown[]): void;
  error(...args: unknown[]): void;
};

declare function setTimeout(handler: () => void, ms: number): { readonly __timer: unique symbol };
declare function clearTimeout(timer: ReturnType<typeof setTimeout>): void;

interface NodeJS_ReadableStream {
  on(event: string, listener: (chunk: unknown) => void): this;
}

interface NodeJS_WritableStream {
  write(chunk: string): boolean;
  end(): void;
  destroyed: boolean;
}

declare module "node:child_process" {
  export interface ChildProcessWithoutNullStreams {
    stdin: NodeJS_WritableStream;
    stdout: NodeJS_ReadableStream;
    stderr: NodeJS_ReadableStream;
    exitCode: number | null;
    once(event: "exit", listener: (code: number | null) => void): this;
    kill(): boolean;
  }
  export function spawn(
    command: string,
    args: string[],
    options: { cwd?: string; stdio?: Array<"pipe"> }
  ): ChildProcessWithoutNullStreams;
}

declare module "node:readline" {
  export interface Interface {
    on(event: "line", listener: (line: string) => void): this;
    close(): void;
  }
  export function createInterface(options: { input: NodeJS_ReadableStream }): Interface;
}

declare module "node:process" {
  export const stdin: NodeJS_ReadableStream;
  export const stdout: NodeJS_WritableStream;
}

declare module "node:url" {
  export function fileURLToPath(url: string): string;
}

declare module "node:path" {
  export function dirname(path: string): string;
  export function join(...parts: string[]): string;
}

declare module "electron" {
  export interface WebPreferences {
    preload?: string;
    contextIsolation?: boolean;
    nodeIntegration?: boolean;
    sandbox?: boolean;
  }
  export class BrowserWindow {
    constructor(options: {
      width?: number;
      height?: number;
      backgroundColor?: string;
      webPreferences?: WebPreferences;
    });
    loadURL(url: string): Promise<void>;
    loadFile(path: string): Promise<void>;
    static getAllWindows(): BrowserWindow[];
  }
  export const app: {
    isPackaged: boolean;
    whenReady(): Promise<void>;
    on(event: string, listener: () => void): void;
    quit(): void;
  };
  export const ipcMain: {
    on(channel: string, listener: (event: { returnValue: unknown }, ...args: unknown[]) => void): void;
    handle(channel: string, listener: (event: unknown, ...args: never[]) => unknown): void;
  };
  export const ipcRenderer: {
    sendSync(channel: string, ...args: unknown[]): unknown;
    invoke(channel: string, ...args: unknown[]): Promise<unknown>;
  };
  export const contextBridge: {
    exposeInMainWorld(key: string, api: unknown): void;
  };
}
