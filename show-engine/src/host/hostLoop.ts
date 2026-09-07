/**
 * The pure request/response/event pump `main.ts` drives over stdio. Takes a
 * `sink` (where response/event lines go) and an already-constructed
 * `ShowEngine` — no `process`, no timers, no `fetch` here, so this file is
 * testable without a real stdin/stdout pair (see the constraint in the
 * task brief: `src/host/` is the only place those primitives may appear,
 * and `hostLoop.ts` itself must stay clean of them). `main.ts` is the only
 * caller that owns `setInterval`/`readline`/`process`.
 *
 * Two independent "did anything change" mechanisms live here, and they are
 * deliberately different:
 *
 *  - `tick()` diffs the CONTENT of the snapshot `engine.tick()` returns
 *    (everything except `revision`, which — unlike a "did this tick change
 *    anything" flag — advances on every single `tick()` call regardless of
 *    whether any input changed, per `ShowEngine.tick()`'s own doc comment)
 *    against the last snapshot this loop published. Only a genuine content
 *    difference (a roster commit, a Mukana settle, anything `tick()` itself
 *    derived differently) republishes. A quiet poll tick must not spam a
 *    `snapshot` event every 250ms.
 *  - `invoke` cannot use that same content diff: an action like
 *    `ohg.look.set` only stages a SELECTION (`LookController.select`) that
 *    `tick()` resolves into a visible `look` later — the snapshot genuinely
 *    does not change shape from the call alone. An operator who just
 *    pressed a button needs to see it reflected in the SAME round trip
 *    regardless, so any action whose `ActionResult` is `{kind:"ok"}`
 *    (a real mutation attempt, as opposed to a refused/error result that
 *    changed nothing) unconditionally republishes a snapshot right after
 *    the response.
 */

import { invokeAction, OHG_ACTIONS, type ActionResult } from "../actions.js";
import { projectControlFields, OHG_FIELD_TEMPLATES, type ControlFieldValue } from "../controlState.js";
import type { ShowEngine } from "../showEngine.js";
import type { ShowSnapshot } from "../showSnapshot.js";
import { decodeRequest, encodeLine, PROTOCOL_VERSION, type Event, type Request, type Response } from "./protocol.js";
import type { LineSink } from "./stdioHostAdapter.js";

export type HostRuntime = {
  engine: ShowEngine;
  generation: number;
  engineVersion: string;
  /** `config.capacity` — compared against a host-reported `capacity` request; see the `capacity` handler. */
  capacity: number;
  sink: LineSink; // where response/event lines go
  now: () => number; // for log timestamps only
};

export class HostLoop {
  private readonly runtime: HostRuntime;
  private mutableShuttingDown = false;

  /** `engine.revision()` as of the last published snapshot/handshake — informational only (the `revision` wire field). */
  private lastPublishedRevision: number | null = null;
  /** Content signature (JSON of the snapshot minus `revision`) as of the last published snapshot/handshake — what `tick()` actually diffs against. */
  private lastPublishedContentKey: string | null = null;

  constructor(runtime: HostRuntime) {
    this.runtime = runtime;
  }

  /** True after a `shutdown` request was handled. */
  get shuttingDown(): boolean {
    return this.mutableShuttingDown;
  }

  /** Emit the unsolicited handshake event. Call once after restore(). */
  announce(): void {
    const snapshot = this.emitHandshake();
    for (const warning of snapshot.restoreWarnings) {
      this.emitLog("warn", warning);
    }
  }

  /** Handle one stdin line. Never throws. Never awaits engine.tick(). */
  handleLine(line: string): void {
    const decoded = decodeRequest(line);
    if (decoded.kind === "malformed") {
      this.send({ id: decoded.id, ok: false, error: { message: decoded.reason } });
      return;
    }
    this.handleRequest(decoded.request);
  }

  /** One engine tick; emits `snapshot` iff revision changed. Never throws. Returns after tick settles. */
  async tick(): Promise<void> {
    let snapshot: ShowSnapshot;
    try {
      snapshot = await this.runtime.engine.tick();
    } catch (error) {
      this.emitLog("error", describeError(error));
      return;
    }
    if (this.contentKey(snapshot) !== this.lastPublishedContentKey) {
      this.publishSnapshot(snapshot);
    }
  }

  // -------------------------------------------------------------------
  // Request dispatch
  // -------------------------------------------------------------------

  private handleRequest(request: Request): void {
    try {
      switch (request.type) {
        case "handshake": {
          this.send({ id: request.id, ok: true });
          this.emitHandshake();
          return;
        }
        case "invoke": {
          const result: ActionResult = invokeAction(this.runtime.engine, request.action, request.args);
          this.send({ id: request.id, ok: true, result });
          if (result.kind === "ok") {
            this.publishSnapshot(this.runtime.engine.snapshot());
          }
          return;
        }
        case "zoomEvent": {
          this.runtime.engine.onZoomEvent(request.event);
          this.send({ id: request.id, ok: true });
          return;
        }
        case "activeSpeaker": {
          this.runtime.engine.onActiveSpeaker(request.participantId);
          this.send({ id: request.id, ok: true });
          return;
        }
        case "capacity": {
          if (request.capacity !== this.runtime.capacity) {
            this.emitLog(
              "warn",
              `host capacity ${request.capacity} differs from config.capacity ${this.runtime.capacity}`
            );
          }
          this.send({ id: request.id, ok: true });
          return;
        }
        case "ping": {
          this.send({ id: request.id, ok: true, revision: this.runtime.engine.revision() });
          return;
        }
        case "shutdown": {
          this.send({ id: request.id, ok: true });
          this.mutableShuttingDown = true;
          return;
        }
      }
    } catch (error) {
      const message = describeError(error);
      this.send({ id: request.id, ok: false, error: { message } });
      this.emitLog("error", message);
    }
  }

  // -------------------------------------------------------------------
  // Snapshot / handshake publication
  // -------------------------------------------------------------------

  private emitHandshake(): ShowSnapshot {
    const snapshot = this.runtime.engine.snapshot();
    const fields = projectControlFields(snapshot);
    this.markPublished(snapshot);
    this.emit({
      event: "handshake",
      protocolVersion: PROTOCOL_VERSION,
      engineVersion: this.runtime.engineVersion,
      generation: this.runtime.generation,
      actions: OHG_ACTIONS,
      fieldTemplates: OHG_FIELD_TEMPLATES,
      snapshot,
      fields
    });
    return snapshot;
  }

  private publishSnapshot(snapshot: ShowSnapshot): void {
    const fields: Record<string, ControlFieldValue> = projectControlFields(snapshot);
    this.markPublished(snapshot);
    this.emit({
      event: "snapshot",
      generation: this.runtime.generation,
      revision: snapshot.revision,
      snapshot,
      fields
    });
  }

  private markPublished(snapshot: ShowSnapshot): void {
    this.lastPublishedRevision = snapshot.revision;
    this.lastPublishedContentKey = this.contentKey(snapshot);
  }

  private contentKey(snapshot: ShowSnapshot): string {
    const { revision, ...rest } = snapshot;
    void revision;
    return JSON.stringify(rest);
  }

  // -------------------------------------------------------------------
  // Wire helpers
  // -------------------------------------------------------------------

  private send(response: Response): void {
    this.runtime.sink(encodeLine(response));
  }

  private emit(event: Event): void {
    this.runtime.sink(encodeLine(event));
  }

  private emitLog(level: "info" | "warn" | "error", message: string): void {
    this.emit({ event: "log", level, message });
  }
}

function describeError(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
