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

type NodeJS_ProcessEnv = Record<string, string | undefined>;

declare namespace NodeJS {
  type ProcessEnv = NodeJS_ProcessEnv;
}

declare const process: {
  execPath: string;
  platform: string;
  argv: string[];
  env: NodeJS_ProcessEnv;
  defaultApp?: boolean;
  resourcesPath?: string;
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
declare function setInterval(handler: () => void, ms: number): { readonly __timer: unique symbol };
declare function clearInterval(timer: ReturnType<typeof setInterval>): void;

declare const fetch: typeof globalThis.fetch;
declare const URL: typeof globalThis.URL;
declare const URLSearchParams: typeof globalThis.URLSearchParams;
declare const Buffer: {
  from(value: string, encoding: string): { toString(encoding: string): string };
};

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
    options: { cwd?: string; env?: Record<string, string | undefined>; stdio?: Array<"pipe"> }
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

declare module "node:buffer" {
  export const Buffer: {
    from(value: string, encoding: string): { toString(encoding: string): string };
    from(value: Uint8Array): { toString(encoding: string): string };
  };
}

declare module "node:crypto" {
  export function randomBytes(size: number): { toString(encoding: "base64url"): string };
  export function createHash(algorithm: string): {
    update(value: string): { digest(encoding: "base64url"): string };
  };
}

declare module "node:fs" {
  export function existsSync(path: string): boolean;
}

declare module "node:module" {
  export function createRequire(url: string): {
    (id: string): unknown;
  };
}

declare module "node:fs/promises" {
  export function mkdir(path: string, options: { recursive: boolean }): Promise<void>;
  export function readFile(path: string, encoding: "utf8"): Promise<string>;
  export function writeFile(path: string, data: string, encoding: "utf8"): Promise<void>;
}

declare const TextEncoder: {
  new (): {
    encode(input: string): Uint8Array;
  };
};

declare namespace Electron {
  interface IpcRendererEvent {
    returnValue: unknown;
  }
}

declare module "electron" {
  export interface WebContents {
    send(channel: string, ...args: unknown[]): void;
  }
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
      title?: string;
      backgroundColor?: string;
      webPreferences?: WebPreferences;
    });
    webContents: WebContents;
    loadURL(url: string): Promise<void>;
    loadFile(path: string): Promise<void>;
    focus(): void;
    static getAllWindows(): BrowserWindow[];
  }
  export const app: {
    isPackaged: boolean;
    getVersion(): string;
    whenReady(): Promise<void>;
    on(event: string, listener: (...args: unknown[]) => void): void;
    quit(): void;
    getPath(name: string): string;
    setAsDefaultProtocolClient(protocol: string, execPath?: string, args?: string[]): boolean;
    requestSingleInstanceLock(): boolean;
  };
  export const ipcMain: {
    on(channel: string, listener: (event: { returnValue: unknown }, ...args: unknown[]) => void): void;
    handle(channel: string, listener: (event: unknown, ...args: never[]) => unknown): void;
  };
  export const ipcRenderer: {
    sendSync(channel: string, ...args: unknown[]): unknown;
    invoke(channel: string, ...args: unknown[]): Promise<unknown>;
    on(channel: string, listener: (...args: unknown[]) => void): void;
    off(channel: string, listener: (...args: unknown[]) => void): void;
  };
  export const contextBridge: {
    exposeInMainWorld(key: string, api: unknown): void;
  };
  export const shell: {
    openExternal(url: string): Promise<void>;
  };
  export const safeStorage: {
    isEncryptionAvailable(): boolean;
    encryptString(value: string): Uint8Array;
    decryptString(value: Uint8Array): string;
  };
}
