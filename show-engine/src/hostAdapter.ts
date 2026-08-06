/**
 * The host adapter port.
 *
 * `show-engine` is host-agnostic: it derives what should be on screen and
 * hands the result to a `HostAdapter`, never touching a specific host's
 * SDK itself. Plans 6–9 each implement this interface once — WinUI,
 * macOS/SwiftUI, and an OBS plugin — against the exact shapes declared
 * here. This file is types only; it must never grow runtime code.
 */

import type { ProgramSource } from "./contracts.js";
import type { Nameplate } from "./lookDirector.js";
import type { QuestionOverlay } from "./overlayDirector.js";

export type HostCapabilities = {
  hasPreviewBus: boolean;
  maxGalleryCells: number;
  transitions: readonly string[];
};

export interface HostAdapter {
  capabilities(): HostCapabilities;
  assignSlot(slot: number, participantId: string | null): void;
  applyLook(lookId: string, boxes: ReadonlyMap<number, number | null>): void;
  setPreview(source: ProgramSource): void;
  cut(): void;
  auto(transitionId?: string): void;
  setGallery(cells: ReadonlyMap<number, number>): void;
  setNameplates(plates: readonly Nameplate[]): void;
  setQuestion(question: QuestionOverlay | null): void;
}
