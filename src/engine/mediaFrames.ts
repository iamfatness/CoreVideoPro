import type { MediaFrameState, Participant } from "../domain/production";

const palette = ["#2f6f62", "#8d6541", "#3b5f75", "#6f5941", "#526a57", "#7b4f4f"];

export function buildMediaFrames(participants: Participant[], tick: number): MediaFrameState[] {
  const participantFrames = participants.flatMap((participant, index): MediaFrameState[] => {
    const lowQuality = participant.health === "low-resolution";
    const videoOff = participant.health === "video-off";
    if (videoOff) {
      return [];
    }

    return [{
      sourceId: `participant:${participant.id}`,
      participantId: participant.id,
      kind: "participant-video",
      frameId: tick * 100 + index,
      width: lowQuality ? 1280 : 1920,
      height: lowQuality ? 720 : 1080,
      fps: lowQuality ? 24 : 30,
      ageMs: participant.health === "recovering" ? 1850 : 90 + index * 24,
      crop: buildSmartCrop(participant, tick),
      dominantColor: palette[index % palette.length],
      label: participant.name,
      health: participant.health
    }];
  });

  const presenter = participants.find((participant) => participant.isScreenSharing);
  if (!presenter) {
    return participantFrames;
  }

  return [
    ...participantFrames,
    {
      sourceId: `screen-share:${presenter.id}:${tick}`,
      participantId: presenter.id,
      kind: "screen-share",
      frameId: tick * 100 + 90,
      width: 1920,
      height: 1080,
      fps: 30,
      ageMs: 75,
      crop: { x: 0, y: 0, width: 1, height: 1 },
      dominantColor: "#dfe8e2",
      label: `${presenter.name} screen share`,
      health: "live"
    }
  ];
}

export function getFrameForParticipant(frames: MediaFrameState[], participantId: string) {
  return frames.find((frame) => frame.kind === "participant-video" && frame.participantId === participantId);
}

function buildSmartCrop(participant: Participant, tick: number) {
  const drift = ((tick % 5) - 2) * 0.01;
  const cropSize = participant.isActiveSpeaker ? 0.78 : 0.86;

  return {
    x: clamp(0.5 - cropSize / 2 + drift),
    y: participant.role === "Presenter" ? 0.08 : 0.06,
    width: cropSize,
    height: 0.86
  };
}

function clamp(value: number) {
  return Math.max(0, Math.min(1, value));
}
