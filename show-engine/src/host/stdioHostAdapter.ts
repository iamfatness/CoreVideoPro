/**
 * `HostAdapter` over a plain line sink (stdout, a socket write, a test
 * array — anything that can accept a string). Every `HostAdapter` call is
 * translated 1:1 into a `hostCommand` event and handed to `sink`; nothing
 * else happens here — no batching, no host-specific interpretation. The
 * actual line encoding (including the Map -> pair-array conversion for
 * `applyLook`'s `boxes` and `setGallery`'s cells) is `encodeLine`'s job in
 * `./protocol.js`; this adapter passes `ReadonlyMap` values through
 * untouched and lets the shared replacer do the serialization, so there is
 * exactly one place in the codebase that knows how a Map goes over the wire.
 */

import type { HostAdapter, HostCapabilities, LookPlacement } from "../hostAdapter.js";
import type { ProgramSource } from "../contracts.js";
import type { Nameplate } from "../lookDirector.js";
import type { QuestionOverlay } from "../overlayDirector.js";
import { encodeLine, type HostCommandName } from "./protocol.js";

export type LineSink = (line: string) => void;

export const WINDOWS_SHELL_CAPABILITIES: HostCapabilities = {
  hasPreviewBus: true,
  maxGalleryCells: 16,
  transitions: ["cut", "fade", "dip", "wipe"]
};

export class StdioHostAdapter implements HostAdapter {
  private readonly sink: LineSink;
  private readonly generation: number;
  private readonly hostCapabilities: HostCapabilities;

  private mutableSeq: number;

  constructor(deps: { sink: LineSink; generation: number; capabilities: HostCapabilities }) {
    this.sink = deps.sink;
    this.generation = deps.generation;
    this.hostCapabilities = deps.capabilities;
    this.mutableSeq = 0;
  }

  /** Monotonic per instance, starts at 1. */
  get seq(): number {
    return this.mutableSeq;
  }

  private emit(name: HostCommandName, args: unknown[]): void {
    this.mutableSeq += 1;
    this.sink(
      encodeLine({ event: "hostCommand", generation: this.generation, seq: this.mutableSeq, name, args })
    );
  }

  capabilities(): HostCapabilities {
    return this.hostCapabilities;
  }

  assignSlot(slot: number, participantId: string | null): void {
    this.emit("assignSlot", [slot, participantId]);
  }

  applyLook(placement: LookPlacement): void {
    this.emit("applyLook", [placement]);
  }

  setPreview(source: ProgramSource): void {
    this.emit("setPreview", [source]);
  }

  cut(): void {
    this.emit("cut", []);
  }

  auto(transitionId?: string): void {
    this.emit("auto", [transitionId ?? null]);
  }

  setGallery(cells: ReadonlyMap<number, number>): void {
    this.emit("setGallery", [cells]);
  }

  setNameplates(plates: readonly Nameplate[]): void {
    this.emit("setNameplates", [plates]);
  }

  setQuestion(question: QuestionOverlay | null): void {
    this.emit("setQuestion", [question]);
  }
}
