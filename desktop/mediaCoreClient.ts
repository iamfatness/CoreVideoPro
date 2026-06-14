/**
 * Child-process supervisor for the media core. The Electron main process owns
 * one of these: it spawns the core (Track B's native binary, or the Node stub),
 * monitors it over stdio JSON-RPC, isolates crashes, restarts with backoff, and
 * performs the capability handshake that populates `mediaCoreProfile`.
 */
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { createInterface, type Interface } from "node:readline";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import type { MediaCoreHealth, NativeMediaCoreCommand, NativeMediaCoreProfile, NativeMediaCoreStateSnapshot } from "../src/engine/nativeMediaCoreProtocol";
import type { ZoomMediaSpineNativeSnapshot } from "../src/engine/zoomMediaSpineNativeSync";
import type { ZoomMediaSpineSyncPayload } from "../src/engine/zoomMediaSpineSync";
import type { ZoomVideoFrame } from "../src/engine/zoomVideoFrames";
import type { CoreRequest, CoreResponse } from "./coreProtocol.ts";
import { parseCoreEvent, parseCoreResponse } from "./coreProtocol.ts";

export type MediaCoreSupervisorOptions = {
  /** Executable to spawn. Defaults to the current Node binary. */
  command?: string;
  /** Args. Defaults to running the bundled Node stub via type-stripping. */
  args?: string[];
  cwd?: string;
  requestTimeoutMs?: number;
  /** Max automatic restarts after unexpected exits before giving up. */
  maxRestarts?: number;
  /** Called whenever the core (re)announces a profile. */
  onProfile?: (profile: NativeMediaCoreProfile) => void;
  /** Called on each unexpected exit (after crash isolation, before restart). */
  onCrash?: (info: { code: number | null; restartCount: number }) => void;
  /** Called for unsolicited decoded Zoom participant frames from the core. */
  onZoomVideoFrame?: (frame: ZoomVideoFrame) => void;
};

type Pending = {
  resolve: (response: CoreResponse) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout>;
};

const STUB_PATH = join(dirname(fileURLToPath(import.meta.url)), "coreStub.ts");

export class MediaCoreSupervisor {
  private child: ChildProcessWithoutNullStreams | undefined;
  private lines: Interface | undefined;
  private readonly pending = new Map<string, Pending>();
  private readonly command: string;
  private readonly args: string[];
  private readonly cwd?: string;
  private readonly requestTimeoutMs: number;
  private readonly maxRestarts: number;
  private nextId = 1;
  private restarts = 0;
  private stopped = false;
  private recovering = false;
  private syncInFlight = false;
  private profile: NativeMediaCoreProfile | undefined;

  constructor(private readonly options: MediaCoreSupervisorOptions = {}) {
    this.command = options.command ?? process.execPath;
    this.args = options.args ?? [STUB_PATH];
    this.cwd = options.cwd;
    this.requestTimeoutMs = options.requestTimeoutMs ?? 4000;
    this.maxRestarts = options.maxRestarts ?? 5;
  }

  /** Spawn the core and complete the capability handshake. */
  async start(): Promise<NativeMediaCoreProfile | undefined> {
    this.stopped = false;
    this.spawnChild();
    this.profile = await this.handshake();
    return this.profile;
  }

  get restartCount(): number {
    return this.restarts;
  }

  get running(): boolean {
    return this.child !== undefined && this.child.exitCode === null && !this.stopped;
  }

  getProfile(): NativeMediaCoreProfile | undefined {
    return this.profile;
  }

  getHealth(): MediaCoreHealth {
    return { restartCount: this.restarts, recovering: this.recovering, stopped: this.stopped };
  }

  async handshake(): Promise<NativeMediaCoreProfile | undefined> {
    const response = await this.send({ id: this.createId(), type: "handshake" });
    if (response.ok && response.type === "handshake") {
      this.recovering = false;
      this.profile = response.profile;
      this.options.onProfile?.(response.profile);
      return response.profile;
    }
    return undefined;
  }

  async ping(): Promise<boolean> {
    const response = await this.send({ id: this.createId(), type: "ping" });
    return response.ok && response.type === "ping";
  }

  async syncMediaCore(commands: NativeMediaCoreCommand[], elapsedMs: number): Promise<NativeMediaCoreStateSnapshot> {
    if (this.syncInFlight) {
      throw new Error("media-core sync in flight; skipped for backpressure");
    }
    this.syncInFlight = true;
    try {
      const response = await this.send({ id: this.createId(), type: "media-core-sync", commands, elapsedMs });
      if (response.ok && response.type === "media-core-sync") {
        return response.snapshot;
      }
      const message = response.ok ? "Unexpected response type." : response.error.message;
      throw new Error(`media-core sync failed: ${message}`);
    } finally {
      this.syncInFlight = false;
    }
  }

  async syncZoomMediaSpine(spinePayload: ZoomMediaSpineSyncPayload, elapsedMs: number): Promise<ZoomMediaSpineNativeSnapshot> {
    const response = await this.send({ id: this.createId(), type: "zoom-media-spine-sync", spinePayload, elapsedMs });
    if (response.ok && response.type === "zoom-media-spine-sync") {
      return response.spineSnapshot;
    }
    const message = response.ok ? "Unexpected response type." : response.error.message;
    throw new Error(`zoom-media-spine sync failed: ${message}`);
  }

  /** Test hook: ask the core to crash, exercising restart. */
  async forceCrash(): Promise<void> {
    try {
      await this.send({ id: this.createId(), type: "__crash" });
    } catch {
      // The crash rejects the in-flight request — expected.
    }
  }

  stop(): void {
    this.stopped = true;
    this.teardownChild(new Error("Supervisor stopped."));
    this.child?.kill();
    this.child = undefined;
  }

  private spawnChild(): void {
    const child = spawn(this.command, this.args, { cwd: this.cwd, stdio: ["pipe", "pipe", "pipe"] });
    this.child = child;
    this.lines = createInterface({ input: child.stdout });
    this.lines.on("line", (line: string) => this.onLine(line));
    child.once("exit", (code) => this.onExit(code));
  }

  private onLine(line: string): void {
    const event = parseCoreEvent(line);
    if (event) {
      this.options.onZoomVideoFrame?.(event.frame);
      return;
    }

    const response = parseCoreResponse(line);
    if (!response) {
      return;
    }
    const pending = this.pending.get(response.id);
    if (!pending) {
      return;
    }
    this.pending.delete(response.id);
    clearTimeout(pending.timer);
    pending.resolve(response);
  }

  private onExit(code: number | null): void {
    const crashError = new Error(`media core exited with code ${code ?? "unknown"}.`);
    this.rejectAll(crashError);
    this.lines?.close();
    this.lines = undefined;

    if (this.stopped) {
      return;
    }

    // Crash isolation: an unexpected exit never takes the host down; we restart.
    this.restarts += 1;
    this.recovering = true;
    this.options.onCrash?.({ code, restartCount: this.restarts });
    if (this.restarts > this.maxRestarts) {
      this.child = undefined;
      return;
    }
    this.spawnChild();
    void this.handshake().catch(() => undefined);
  }

  private teardownChild(error: Error): void {
    this.rejectAll(error);
    this.lines?.close();
    this.lines = undefined;
  }

  private rejectAll(error: Error): void {
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pending.clear();
  }

  private send(request: CoreRequest): Promise<CoreResponse> {
    return new Promise<CoreResponse>((resolve, reject) => {
      const child = this.child;
      if (!child || child.stdin.destroyed) {
        reject(new Error("media core is not running."));
        return;
      }
      const timer = setTimeout(() => {
        this.pending.delete(request.id);
        reject(new Error(`media core request ${request.id} timed out.`));
      }, this.requestTimeoutMs);
      this.pending.set(request.id, { resolve, reject, timer });
      child.stdin.write(`${JSON.stringify(request)}\n`);
    });
  }

  private createId(): string {
    const id = `core-${this.nextId}`;
    this.nextId += 1;
    return id;
  }
}
