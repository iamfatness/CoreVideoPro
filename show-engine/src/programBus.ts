/**
 * The program/preview bus — a software model of the replacement for a
 * hardware switcher's mix/effect stage. It tracks *what* is currently on
 * air and what is cued up next; rendering that decision onto video is the
 * host's job in a later plan, not this module's. This module also tracks
 * the current active speaker and, when follow mode is on, drives program
 * off of it — while giving certain roles (e.g. an ASL interpreter, who is
 * signing continuously while a panelist talks) an accessibility exemption
 * from ever stealing the shot.
 */

import type { ProgramSource, Role } from "./contracts.js";
import { DEFAULT_SKIP_ROLES, programSourcesEqual } from "./contracts.js";
import { shouldFollowSpeaker } from "./speakerGate.js";

export type ProgramState = {
  program: ProgramSource;
  preview: ProgramSource;
  activeSpeakerFollow: boolean;
  activeSpeakerId: string | null;
};

export class ProgramBus {
  private program: ProgramSource = { kind: "black" };
  private preview: ProgramSource = { kind: "black" };
  private activeSpeakerFollow = false;
  private activeSpeakerId: string | null = null;
  private readonly skipRoles: readonly Role[];

  constructor(options?: { skipRoles?: readonly Role[] }) {
    this.skipRoles = options?.skipRoles ?? DEFAULT_SKIP_ROLES;
  }

  state(): ProgramState {
    return {
      program: { ...this.program },
      preview: { ...this.preview },
      activeSpeakerFollow: this.activeSpeakerFollow,
      activeSpeakerId: this.activeSpeakerId
    };
  }

  setPreview(source: ProgramSource): void {
    this.preview = { ...source };
  }

  /**
   * `cut()` and `auto()` are modeled identically on purpose: they differ
   * only in the transition the host performs (a hard cut versus a timed
   * dissolve), which is outside this module's concern. Do not give them
   * diverging implementations.
   */
  cut(): void {
    this.swap();
  }

  auto(): void {
    this.swap();
  }

  private swap(): void {
    const outgoing = this.program;
    this.program = this.preview;
    this.preview = outgoing;
  }

  directCut(source: ProgramSource): void {
    this.preview = this.program;
    this.program = { ...source };
  }

  setActiveSpeakerFollow(on: boolean): void {
    this.activeSpeakerFollow = on;
  }

  onActiveSpeaker(participantId: string, role: Role): boolean {
    if (!shouldFollowSpeaker(role, this.skipRoles)) {
      return false;
    }

    this.activeSpeakerId = participantId;

    if (!this.activeSpeakerFollow) {
      return false;
    }

    if (programSourcesEqual(this.program, { kind: "activeSpeaker" })) {
      return false;
    }

    this.directCut({ kind: "activeSpeaker" });
    return true;
  }
}
