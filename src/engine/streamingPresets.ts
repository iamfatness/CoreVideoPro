import type { OutputDestination } from "../domain/production";

export type StreamingPreset = {
  id: string;
  name: string;
  protocol: OutputDestination["protocol"];
  endpoint: string;
  latencyMs: number;
  requiresStreamKey: boolean;
  helpText: string;
};

export const streamingPresets: StreamingPreset[] = [
  {
    id: "youtube",
    name: "YouTube",
    protocol: "RTMP",
    endpoint: "rtmp://a.rtmp.youtube.com/live2",
    latencyMs: 2400,
    requiresStreamKey: true,
    helpText: "Paste the stream key from YouTube Studio > Go live."
  },
  {
    id: "twitch",
    name: "Twitch",
    protocol: "RTMP",
    endpoint: "rtmp://live.twitch.tv/app",
    latencyMs: 2200,
    requiresStreamKey: true,
    helpText: "Use the primary stream key from your Twitch dashboard."
  },
  {
    id: "facebook",
    name: "Facebook Live",
    protocol: "RTMP",
    endpoint: "rtmps://live-api-s.facebook.com:443/rtmp/",
    latencyMs: 2600,
    requiresStreamKey: true,
    helpText: "Copy the stream key from the Facebook Live producer."
  },
  {
    id: "custom-rtmp",
    name: "Custom RTMP",
    protocol: "RTMP",
    endpoint: "rtmp://",
    latencyMs: 2000,
    requiresStreamKey: true,
    helpText: "Enter the ingest URL and stream key from your provider."
  }
];

export function getStreamingPreset(presetId: string): StreamingPreset | undefined {
  return streamingPresets.find((preset) => preset.id === presetId);
}

/**
 * Build a new output destination from a streaming preset, assigning a unique
 * id and a numbered display name when a destination from the same preset is
 * already present (e.g. a second "Twitch 2" backup feed).
 */
export function createDestinationFromPreset(
  preset: StreamingPreset,
  existing: OutputDestination[]
): OutputDestination {
  const sameFamily = existing.filter((destination) => destination.id === preset.id || destination.id.startsWith(`${preset.id}-`));
  const ordinal = sameFamily.length + 1;
  const id = sameFamily.length === 0 ? preset.id : `${preset.id}-${ordinal}`;
  const name = sameFamily.length === 0 ? preset.name : `${preset.name} ${ordinal}`;

  return {
    id,
    name,
    protocol: preset.protocol,
    enabled: true,
    latencyMs: preset.latencyMs,
    endpoint: preset.endpoint,
    streamKey: preset.requiresStreamKey ? "" : undefined
  };
}

export function addDestinationFromPreset(
  presetId: string,
  existing: OutputDestination[]
): OutputDestination[] {
  const preset = getStreamingPreset(presetId);
  if (!preset) {
    return existing;
  }

  return [...existing, createDestinationFromPreset(preset, existing)];
}
