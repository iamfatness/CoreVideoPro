import type { Participant } from "../domain/production";

export type AudioTrackPlan = {
  id: string;
  label: string;
  kind: "master" | "iso";
  participantId?: string;
  channels: 1 | 2;
  muted: boolean;
};

export type MultitrackPlan = {
  tracks: AudioTrackPlan[];
  trackCount: number;
  totalChannels: number;
  estimatedDiskRateMBps: number;
  summary: string;
  warnings: string[];
};

// 48 kHz / 24-bit PCM is 144 kB/s per channel (48000 * 3 bytes).
const MB_PER_CHANNEL_SECOND = (48000 * 3) / 1_000_000;
const HIGH_TRACK_COUNT = 8;

/**
 * Plan the multitrack audio bundle for a recording: a stereo master mix plus a
 * mono ISO track per selected participant. Surfaces channel totals, an
 * estimated disk rate, and warnings for silent or missing ISO sources so the
 * operator can confirm the layout before arming the record.
 */
export function planMultitrackAudio(participants: Participant[], isoParticipantIds: string[]): MultitrackPlan {
  const tracks: AudioTrackPlan[] = [
    { id: "master", label: "Program master", kind: "master", channels: 2, muted: false }
  ];

  const warnings: string[] = [];

  isoParticipantIds.forEach((participantId) => {
    const participant = participants.find((candidate) => candidate.id === participantId);

    if (!participant) {
      warnings.push(`ISO track for ${participantId} has no matching source.`);
      return;
    }

    tracks.push({
      id: `iso:${participant.id}`,
      label: `${participant.name} ISO`,
      kind: "iso",
      participantId: participant.id,
      channels: 1,
      muted: participant.isMuted
    });

    if (participant.isMuted) {
      warnings.push(`${participant.name}'s ISO track will be silent while muted.`);
    }
  });

  const totalChannels = tracks.reduce((total, track) => total + track.channels, 0);
  const estimatedDiskRateMBps = Math.round(totalChannels * MB_PER_CHANNEL_SECOND * 100) / 100;

  if (tracks.length > HIGH_TRACK_COUNT) {
    warnings.push(`${tracks.length} audio tracks is a heavy multitrack load; confirm disk headroom.`);
  }

  return {
    tracks,
    trackCount: tracks.length,
    totalChannels,
    estimatedDiskRateMBps,
    summary: `${tracks.length} tracks - ${totalChannels} ch - ${estimatedDiskRateMBps} MB/s`,
    warnings
  };
}
