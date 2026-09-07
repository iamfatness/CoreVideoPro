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

/**
 * Everything `applyLook` hands a host in one call: the look identity, the
 * scene preset it renders through, both chairs (either `null` when this
 * look does not include that chair, or nobody currently fills it), and the
 * guest boxes. `LookResolution` (`./lookDirector.js`) already resolves all
 * five values every tick — this is plumbing what the engine already knows,
 * not a new computation, so a host adapter never has to re-derive
 * `lookId -> scenePreset` or place the chairs from `setNameplates` itself.
 */
export type LookPlacement = {
  lookId: string;
  scenePreset: string;
  hostSlot: number | null;
  readerSlot: number | null;
  boxes: ReadonlyMap<number, number | null>;
};

export interface HostAdapter {
  capabilities(): HostCapabilities;
  assignSlot(slot: number, participantId: string | null): void;
  applyLook(placement: LookPlacement): void;
  setPreview(source: ProgramSource): void;
  cut(): void;
  auto(transitionId?: string): void;
  setGallery(cells: ReadonlyMap<number, number>): void;
  setNameplates(plates: readonly Nameplate[]): void;
  setQuestion(question: QuestionOverlay | null): void;
}
