/**
 * An in-memory `HostAdapter` for tests: records every call it receives
 * instead of driving a real host, so an orchestrator test can assert on
 * exactly what would have been sent.
 */

import type { ProgramSource } from "./contracts.js";
import type { Nameplate } from "./lookDirector.js";
import type { QuestionOverlay } from "./overlayDirector.js";
import type { HostAdapter, HostCapabilities } from "./hostAdapter.js";

export type HostCall =
  | { kind: "assignSlot"; slot: number; participantId: string | null }
  | { kind: "applyLook"; lookId: string; boxes: Array<[number, number | null]> }
  | { kind: "setPreview"; source: ProgramSource }
  | { kind: "cut" }
  | { kind: "auto"; transitionId: string | undefined }
  | { kind: "setGallery"; cells: Array<[number, number]> }
  | { kind: "setNameplates"; plates: Nameplate[] }
  | { kind: "setQuestion"; question: QuestionOverlay | null };

const DEFAULT_CAPABILITIES: HostCapabilities = {
  hasPreviewBus: true,
  maxGalleryCells: 16,
  transitions: ["cut", "fade"]
};

export class MockHost implements HostAdapter {
  private readonly capabilities_: HostCapabilities;
  private readonly calls_: HostCall[] = [];

  constructor(capabilities?: Partial<HostCapabilities>) {
    this.capabilities_ = {
      ...DEFAULT_CAPABILITIES,
      ...capabilities,
      transitions: [...(capabilities?.transitions ?? DEFAULT_CAPABILITIES.transitions)]
    };
  }

  /**
   * A fresh object with a fresh `transitions` array on every call (final
   * review, Minor). This used to hand out the instance's own record by
   * reference — and with no `transitions` override, that array WAS
   * `DEFAULT_CAPABILITIES.transitions`, shared by every `MockHost` ever
   * constructed in the process. A Plan 6 conformance test that pushed a
   * transition onto what it read back would have silently rewritten the
   * defaults for every other host and every other test file in the run.
   */
  capabilities(): HostCapabilities {
    return { ...this.capabilities_, transitions: [...this.capabilities_.transitions] };
  }

  calls(): readonly HostCall[] {
    return this.calls_;
  }

  callsOfKind<K extends HostCall["kind"]>(kind: K): ReadonlyArray<Extract<HostCall, { kind: K }>> {
    return this.calls_.filter(
      (call): call is Extract<HostCall, { kind: K }> => call.kind === kind
    );
  }

  clear(): void {
    this.calls_.length = 0;
  }

  assignSlot(slot: number, participantId: string | null): void {
    this.calls_.push({ kind: "assignSlot", slot, participantId });
  }

  applyLook(lookId: string, boxes: ReadonlyMap<number, number | null>): void {
    this.calls_.push({ kind: "applyLook", lookId, boxes: [...boxes.entries()] });
  }

  setPreview(source: ProgramSource): void {
    this.calls_.push({ kind: "setPreview", source });
  }

  cut(): void {
    this.calls_.push({ kind: "cut" });
  }

  auto(transitionId?: string): void {
    this.calls_.push({ kind: "auto", transitionId });
  }

  setGallery(cells: ReadonlyMap<number, number>): void {
    this.calls_.push({ kind: "setGallery", cells: [...cells.entries()] });
  }

  setNameplates(plates: readonly Nameplate[]): void {
    this.calls_.push({ kind: "setNameplates", plates: [...plates] });
  }

  setQuestion(question: QuestionOverlay | null): void {
    this.calls_.push({ kind: "setQuestion", question });
  }
}
