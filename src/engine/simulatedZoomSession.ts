import { initialParticipants } from "../domain/production";
import type { MeetingState, ZoomJoinRequest, ZoomSessionSnapshot } from "./contracts";
import {
  mapCaptureSnapshot,
  type RawCaptureSnapshot,
  type RawParticipantEvent
} from "./captureSnapshotMapper";

const captionCycle = [
  "The active speaker is framed automatically while the slides remain readable.",
  "Magic Scene is holding the presenter shot until the next speaker settles.",
  "Lower-thirds and captions are adapting around the active participant.",
  "Audio leveling is smoothing each person before the program mix."
];

const seedRawParticipants: RawParticipantEvent[] = initialParticipants.map((participant) => ({
  userId: participant.id,
  displayName: participant.name,
  title: participant.title,
  role: participant.role,
  breakoutRoomId: participant.breakoutRoomId,
  breakoutRoomName: participant.breakoutRoomName,
  muted: participant.isMuted,
  videoOn: participant.health !== "video-off",
  talking: participant.isActiveSpeaker,
  sharingScreen: participant.isScreenSharing,
  audioLevel: participant.audioLevel,
  networkQuality: participant.health === "low-resolution" ? "low" : participant.health === "recovering" ? "recovering" : "good"
}));

export class SimulatedZoomSession {
  private meetingState: MeetingState = "in_meeting";
  private tick = 0;
  private rawParticipants: RawParticipantEvent[] = cloneRawParticipants(seedRawParticipants);

  async join(_request: ZoomJoinRequest): Promise<ZoomSessionSnapshot> {
    this.meetingState = "in_meeting";
    this.tick = 0;
    this.rawParticipants = cloneRawParticipants(seedRawParticipants);
    return this.getSnapshot();
  }

  async leave(): Promise<ZoomSessionSnapshot> {
    this.meetingState = "idle";
    this.rawParticipants = [];
    return this.getSnapshot();
  }

  async advance(): Promise<ZoomSessionSnapshot> {
    if (this.meetingState !== "in_meeting") {
      return this.getSnapshot();
    }

    this.tick += 1;
    const speakerIndex = (this.tick + 1) % seedRawParticipants.length;
    const screenShareActive = this.tick % 4 !== 2;

    this.rawParticipants = seedRawParticipants.map((participant, index) => {
      const talking = index === speakerIndex;
      const recovering = participant.userId === "p3" && this.tick % 3 === 1;

      return {
        ...participant,
        talking,
        sharingScreen: participant.role === "Presenter" && screenShareActive,
        audioLevel: talking ? 84 : Math.max(12, (participant.audioLevel ?? 22) - 10 + ((this.tick + index) % 18)),
        networkQuality: recovering ? "recovering" : participant.networkQuality
      };
    });

    return this.getSnapshot();
  }

  async getParticipants() {
    return this.getSnapshot().then((snapshot) => snapshot.participants);
  }

  async getSnapshot(): Promise<ZoomSessionSnapshot> {
    return mapCaptureSnapshot(this.getRawSnapshot());
  }

  private getRawSnapshot(): RawCaptureSnapshot {
    const activeSpeaker = this.rawParticipants.find((participant) => participant.talking);

    return {
      meetingState: this.meetingState,
      participants: cloneRawParticipants(this.rawParticipants),
      activeSpeakerId: activeSpeaker?.userId,
      caption: captionCycle[this.tick % captionCycle.length],
      tick: this.tick
    };
  }
}

function cloneRawParticipants(participants: RawParticipantEvent[]) {
  return participants.map((participant) => ({ ...participant }));
}
