import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { createInterface, type Interface } from "node:readline";
import type { MediaCoreCommand, MediaCoreRequest, MediaCoreResponse } from "./protocol.js";

type PendingRequest = {
  resolve(response: MediaCoreResponse): void;
  reject(error: Error): void;
};

export type MediaCoreClientOptions = {
  command?: string;
  args?: string[];
  cwd?: string;
  requestTimeoutMs?: number;
};

export class MediaCoreServiceClient {
  private readonly process: ChildProcessWithoutNullStreams;
  private readonly lines: Interface;
  private readonly pending = new Map<string, PendingRequest>();
  private readonly requestTimeoutMs: number;
  private nextId = 1;

  constructor(options: MediaCoreClientOptions = {}) {
    this.requestTimeoutMs = options.requestTimeoutMs ?? 5000;
    this.process = spawn(options.command ?? process.execPath, options.args ?? ["dist/service.js"], {
      cwd: options.cwd,
      stdio: ["pipe", "pipe", "pipe"]
    });
    this.lines = createInterface({ input: this.process.stdout });

    this.lines.on("line", (line) => this.handleResponseLine(line));
    this.process.once("exit", (code) => {
      this.rejectAll(new Error(`native-core exited with code ${code ?? "unknown"}.`));
    });
    this.process.stderr.on("data", (chunk: Buffer) => {
      const message = chunk.toString().trim();
      if (message.length > 0) {
        this.rejectAll(new Error(`native-core stderr: ${message}`));
      }
    });
  }

  sync(commands: MediaCoreCommand[]) {
    return this.send({ id: this.createRequestId(), type: "sync", commands });
  }

  snapshot() {
    return this.send({ id: this.createRequestId(), type: "snapshot" });
  }

  tick(elapsedMs: number) {
    return this.send({ id: this.createRequestId(), type: "tick", elapsedMs });
  }

  close() {
    this.lines.close();
    this.process.stdin.end();
    this.process.kill();
  }

  private send(request: MediaCoreRequest) {
    return new Promise<MediaCoreResponse>((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.pending.delete(request.id);
        reject(new Error(`native-core request ${request.id} timed out.`));
      }, this.requestTimeoutMs);

      this.pending.set(request.id, {
        resolve: (response) => {
          clearTimeout(timeout);
          resolve(response);
        },
        reject: (error) => {
          clearTimeout(timeout);
          reject(error);
        }
      });
      this.process.stdin.write(`${JSON.stringify(request)}\n`);
    });
  }

  private handleResponseLine(line: string) {
    const response = JSON.parse(line) as MediaCoreResponse;
    const pending = this.pending.get(response.id);
    if (!pending) {
      return;
    }

    this.pending.delete(response.id);
    pending.resolve(response);
  }

  private rejectAll(error: Error) {
    this.pending.forEach((pending) => pending.reject(error));
    this.pending.clear();
  }

  private createRequestId() {
    const id = `native-core-${this.nextId}`;
    this.nextId += 1;
    return id;
  }
}
