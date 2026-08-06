/**
 * The overlay director — derives everything textual that goes on screen: a
 * nameplate under each occupied position, and the current audience question
 * when the operator has it up. This is the successor to the legacy SPX
 * client: CoreVideo Pro renders these through its own lower-third/overlay
 * engine now, so there is no template name, no field payload, and no
 * rundown transport, just the derived state a renderer needs.
 *
 * `update` is meant to be called every tick by a future orchestrator. Its
 * boolean return — whether the derived state actually changed — is the
 * whole point of the class: re-rendering an identical lower third would
 * restart its on-air animation while a person is mid-sentence, so change
 * detection compares the derived state structurally, field by field, never
 * by object identity.
 */

import type { MukanaQuestion } from "./contracts.js";
import type { LookResolution, Nameplate } from "./lookDirector.js";

export type QuestionOverlay = {
  askerName: string;
  text: string;
  tag: string;
  votes: number;
};

export type OverlayState = {
  nameplates: Nameplate[];
  question: QuestionOverlay | null;
};

export type OverlayInput = {
  look: LookResolution | null;
  question: MukanaQuestion | null;
  questionVisible: boolean;
};

function emptyState(): OverlayState {
  return { nameplates: [], question: null };
}

function cloneNameplates(nameplates: readonly Nameplate[]): Nameplate[] {
  return nameplates.map((plate) => ({ ...plate, position: { ...plate.position } }));
}

function deriveQuestion(input: OverlayInput): QuestionOverlay | null {
  if (!input.questionVisible) return null;
  if (input.question === null) return null;
  if (input.question.text === "") return null;
  const { askerName, text, tag, votes } = input.question;
  return { askerName, text, tag, votes };
}

function deriveState(input: OverlayInput): OverlayState {
  const nameplates = input.look === null ? [] : cloneNameplates(input.look.nameplates);
  const question = deriveQuestion(input);
  return { nameplates, question };
}

function nameplatesEqual(a: readonly Nameplate[], b: readonly Nameplate[]): boolean {
  if (a.length !== b.length) return false;
  for (let index = 0; index < a.length; index += 1) {
    const left = a[index];
    const right = b[index];
    if (left === undefined || right === undefined) return false;
    if (
      left.position.kind !== right.position.kind ||
      (left.position.kind === "box" &&
        right.position.kind === "box" &&
        left.position.box !== right.position.box) ||
      left.slot !== right.slot ||
      left.name !== right.name ||
      left.location !== right.location ||
      left.tone !== right.tone
    ) {
      return false;
    }
  }
  return true;
}

function questionsEqual(a: QuestionOverlay | null, b: QuestionOverlay | null): boolean {
  if (a === null || b === null) return a === b;
  return a.askerName === b.askerName && a.text === b.text && a.tag === b.tag && a.votes === b.votes;
}

function statesEqual(a: OverlayState, b: OverlayState): boolean {
  return nameplatesEqual(a.nameplates, b.nameplates) && questionsEqual(a.question, b.question);
}

/**
 * Holds the currently published overlay state and reports, on each
 * `update`, whether that state actually changed. No I/O: purely an
 * in-memory derivation over its last known state.
 */
export class OverlayDirector {
  private current: OverlayState = emptyState();

  state(): OverlayState {
    return {
      nameplates: cloneNameplates(this.current.nameplates),
      question: this.current.question === null ? null : { ...this.current.question }
    };
  }

  update(input: OverlayInput): boolean {
    const next = deriveState(input);
    const changed = !statesEqual(this.current, next);
    this.current = next;
    return changed;
  }

  reset(): void {
    this.current = emptyState();
  }
}
