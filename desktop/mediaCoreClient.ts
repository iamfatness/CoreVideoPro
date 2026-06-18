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
import type { CaptureDeviceState } from "../src/domain/production";
import type { RawCaptureSnapshot } from "../src/engine/captureSnapshotMapper";
import type { ZoomJoinRequest } from "../src/engine/contracts";
import type { MediaCoreHealth, NativeMediaCoreCommand, NativeMediaCoreProfile, NativeMediaCoreStateSnapshot } from "../src/engine/nativeMediaCoreProtocol";
import type { ZoomMediaSpineNativeSnapshot } from "../src/engine/zoomMediaSpineNativeSync";
import type { ZoomMediaSpineSyncPayload } from "../src/engine/zoomMediaSpineSync";
import type { ZoomVideoFrame } from "../src/engine/zoomVideoFrames";
import type { CoreRequest, CoreResponse } from "./coreProtocol.ts";
import { parseCoreEvent, parseCoreResponse } from "./coreProtocol.ts";
import { isNativeMediaCoreWireState, mapNativeWireStateToSnapshot } from "./nativeMediaCoreStateMapper.ts";

export type MediaCoreSupervisorOptions = {
  /** Executable to spawn. Defaults to the current Node binary. */
  command?: string;
  /** Args. Defaults to running the bundled Node stub via type-stripping. */
  args?: string[];
  cwd?: string;
  /** Extra env vars merged into the child process (after the host process env). */
  env?: NodeJS.ProcessEnv;
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

/** Drain unsolicited zoom-video-frame events at up to 60fps (pro production path). */
const ZOOM_FRAME_DRAIN_INTERVAL_MS = (() => {
  const configured = Number(process.env.COREVIDEO_ZOOM_FRAME_DRAIN_INTERVAL_MS ?? "16");
  if (!Number.isFinite(configured) || configured <= 0) {
    return 16;
  }
  return Math.min(configured, 1000);
})();

export class MediaCoreSupervisor {
  private child: ChildProcessWithoutNullStreams | undefined;
  private lines: Interface | undefined;
  private readonly pending = new Map<string, Pending>();
  private readonly command: string;
  private readonly args: string[];
  private readonly cwd?: string;
  private readonly childEnv: NodeJS.ProcessEnv;
  private readonly requestTimeoutMs: number;
  private readonly maxRestarts: number;
  private nextId = 1;
  private restarts = 0;
  private stopped = false;
  private recovering = false;
  private syncInFlight = false;
  private profile: NativeMediaCoreProfile | undefined;
  private frameDrainTimer: ReturnType<typeof setInterval> | undefined;
  private syncFrameNumber = 0;

  constructor(private readonly options: MediaCoreSupervisorOptions = {}) {
    this.command = options.command ?? process.execPath;
    this.args = options.args ?? [STUB_PATH];
    this.cwd = options.cwd;
    this.childEnv = { ...process.env, ...options.env };
    this.requestTimeoutMs = options.requestTimeoutMs ?? 4000;
    this.maxRestarts = options.maxRestarts ?? 5;
  }

  /** Spawn the core and complete the capability handshake. */
  async start(): Promise<NativeMediaCoreProfile | undefined> {
    this.stopped = false;
    this.spawnChild();
    this.profile = await this.handshake();
    this.startFrameDrain();
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
      if (!response.ok) {
        throw new Error(`media-core sync failed: ${response.error.message}`);
      }

      if (response.type === "media-core-sync" && "snapshot" in response && !isNativeMediaCoreWireState(response.snapshot)) {
        return response.snapshot;
      }

      const wire =
        response.type === "media-core-sync" && "snapshot" in response
          ? response.snapshot
          : "state" in response
            ? response.state
            : undefined;
      if (isNativeMediaCoreWireState(wire)) {
        this.syncFrameNumber += 1;
        return mapNativeWireStateToSnapshot(commands, elapsedMs, this.syncFrameNumber, wire);
      }

      throw new Error("media-core sync failed: Unexpected response type.");
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

  async joinZoom(payload: ZoomJoinRequest): Promise<RawCaptureSnapshot> {
    const response = await this.send({ id: this.createId(), type: "zoom-join", payload });
    if (response.ok && response.type === "zoom-join") {
      return response.snapshot;
    }
    const message = response.ok ? "Unexpected response type." : response.error.message;
    throw new Error(`zoom join failed: ${message}`);
  }

  async leaveZoom(): Promise<RawCaptureSnapshot> {
    const response = await this.send({ id: this.createId(), type: "zoom-leave" });
    if (response.ok && response.type === "zoom-leave") {
      return response.snapshot;
    }
    const message = response.ok ? "Unexpected response type." : response.error.message;
    throw new Error(`zoom leave failed: ${message}`);
  }

  async getZoomSnapshot(): Promise<RawCaptureSnapshot> {
    const response = await this.send({ id: this.createId(), type: "zoom-snapshot" });
    if (response.ok && response.type === "zoom-snapshot") {
      return response.snapshot;
    }
    const message = response.ok ? "Unexpected response type." : response.error.message;
    throw new Error(`zoom snapshot failed: ${message}`);
  }

  async listCaptureDevices(): Promise<CaptureDeviceState[]> {
    return this.sendForCaptureDevices({ id: this.createId(), type: "list-capture-devices" });
  }

  async selectCaptureInput(deviceId: string, inputId: string): Promise<CaptureDeviceState[]> {
    return this.sendForCaptureDevices({
      id: this.createId(),
      type: "select-capture-input",
      payload: { deviceId, inputId }
    });
  }

  async setCaptureAudioSyncOffset(deviceId: string, offsetMs: number): Promise<CaptureDeviceState[]> {
    return this.sendForCaptureDevices({
      id: this.createId(),
      type: "set-capture-audio-sync-offset",
      payload: { deviceId, offsetMs }
    });
  }

  async connectCaptureDevice(deviceId: string): Promise<CaptureDeviceState[]> {
    return this.sendForCaptureDevices({
      id: this.createId(),
      type: "connect-capture-device",
      payload: { deviceId }
    });
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
    this.stopFrameDrain();
    this.teardownChild(new Error("Supervisor stopped."));
    this.child?.kill();
    this.child = undefined;
  }

  private startFrameDrain(): void {
    this.stopFrameDrain();
    this.frameDrainTimer = setInterval(() => {
      if (!this.running) {
        return;
      }
      void this.ping().catch(() => undefined);
    }, ZOOM_FRAME_DRAIN_INTERVAL_MS);
  }

  private stopFrameDrain(): void {
    if (this.frameDrainTimer) {
      clearInterval(this.frameDrainTimer);
      this.frameDrainTimer = undefined;
    }
  }

  private spawnChild(): void {
    const child = spawn(this.command, this.args, { cwd: this.cwd, env: this.childEnv, stdio: ["pipe", "pipe", "pipe"] });
    this.child = child;
    this.lines = createInterface({ input: child.stdout });
    this.lines.on("line", (line: string) => this.onLine(line));
    child.stdin.on("error", (error: Error) => this.rejectAll(error));
    child.once("exit", (code) => this.onExit(code));
  }

  private onLine(line: string): void {
    const event = parseCoreEvent(line);
    if (event) {
      if (event.type === "zoom-video-frame") {
        this.options.onZoomVideoFrame?.(event.frame);
      }
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
      const rejectWrite = (error: Error) => {
        const pending = this.pending.get(request.id);
        if (!pending) {
          return;
        }
        this.pending.delete(request.id);
        clearTimeout(pending.timer);
        pending.reject(error);
      };
      try {
        child.stdin.write(`${JSON.stringify(request)}\n`, (error?: Error | null) => {
          if (error) {
            rejectWrite(error);
          }
        });
      } catch (error) {
        rejectWrite(error instanceof Error ? error : new Error(String(error)));
      }
    });
  }

  private createId(): string {
    const id = `core-${this.nextId}`;
    this.nextId += 1;
    return id;
  }

  private async sendForCaptureDevices(
    request: Extract<CoreRequest, { type: "list-capture-devices" | "select-capture-input" | "set-capture-audio-sync-offset" | "connect-capture-device" }>
  ): Promise<CaptureDeviceState[]> {
    const response = await this.send(request);
    if (response.ok && response.type === "capture-devices") {
      return response.devices;
    }
    const message = response.ok ? "Unexpected capture response." : response.error.message;
    throw new Error(`capture device request failed: ${message}`);
  }
}
