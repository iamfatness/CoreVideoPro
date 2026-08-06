/**
 * Participant ingest cache.
 * Turns the host's discrete Zoom roster events into a stable published
 * snapshot. Replaces the patch's ZoomOSC listeners plus `Zoom_Cached_Data`:
 * the working/published double buffer means a bulk roster replacement is never
 * observed half-applied by downstream stages.
 */

import type { Participant } from "./contracts.js";

export type ZoomEvent =
  | { kind: "roster"; participants: Participant[] }
  | { kind: "joined"; participant: Participant }
  | { kind: "left"; participantId: string }
  | { kind: "video"; participantId: string; on: boolean }
  | { kind: "audio"; participantId: string; on: boolean }
  | { kind: "hand"; participantId: string; raised: boolean }
  | { kind: "renamed"; participantId: string; rawName: string };

export class ZoomIngest {
  private working = new Map<string, Participant>();
  private published: readonly Participant[] = [];
  private isDirty = false;

  get dirty(): boolean {
    return this.isDirty;
  }

  apply(event: ZoomEvent): void {
    switch (event.kind) {
      case "roster": {
        const nextWorking = new Map(event.participants.map((p) => [p.participantId, { ...p }]));
        if (!sameRoster(this.working, nextWorking)) {
          this.working = nextWorking;
          this.isDirty = true;
        }
        return;
      }
      case "joined": {
        const current = this.working.get(event.participant.participantId);
        if (current === undefined || changed(current, event.participant)) {
          this.working.set(event.participant.participantId, { ...event.participant });
          this.isDirty = true;
        }
        return;
      }
      case "left": {
        // Intentionally leave audioOn and handRaised untouched so they can be restored on reconnect.
        this.mutate(event.participantId, { online: false, videoOn: false });
        return;
      }
      case "video": {
        this.mutate(event.participantId, { videoOn: event.on });
        return;
      }
      case "audio": {
        this.mutate(event.participantId, { audioOn: event.on });
        return;
      }
      case "hand": {
        this.mutate(event.participantId, { handRaised: event.raised });
        return;
      }
      case "renamed": {
        this.mutate(event.participantId, { rawName: event.rawName });
        return;
      }
      default: {
        const _exhaustive: never = event;
        return _exhaustive;
      }
    }
  }

  /** Publish the working set. Returns whether the published snapshot changed. */
  commit(): boolean {
    if (!this.isDirty) return false;
    this.published = [...this.working.values()]
      .map((p) => ({ ...p }))
      .sort((a, b) => a.participantId.localeCompare(b.participantId));
    this.isDirty = false;
    return true;
  }

  snapshot(): readonly Participant[] {
    return this.published.map((p) => ({ ...p }));
  }

  private mutate(participantId: string, patch: Partial<Participant>): void {
    const current = this.working.get(participantId);
    if (current === undefined) return;

    const next = { ...current, ...patch };
    if (!changed(current, next)) return;

    this.working.set(participantId, next);
    this.isDirty = true;
  }
}

function changed(a: Participant, b: Participant): boolean {
  return (
    a.rawName !== b.rawName ||
    a.online !== b.online ||
    a.videoOn !== b.videoOn ||
    a.audioOn !== b.audioOn ||
    a.handRaised !== b.handRaised ||
    a.zoomRole !== b.zoomRole
  );
}

function sameRoster(
  a: Map<string, Participant>,
  b: Map<string, Participant>
): boolean {
  if (a.size !== b.size) return false;
  for (const [id, aParticipant] of a) {
    const bParticipant = b.get(id);
    if (bParticipant === undefined || changed(aParticipant, bParticipant)) {
      return false;
    }
  }
  return true;
}
