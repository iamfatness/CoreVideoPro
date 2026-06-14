import type { OutputDestination, RecordingSettings } from "../domain/production";

export type OutputPreflightResult = {
  ready: boolean;
  destinations: OutputDestination[];
  errors: string[];
  warnings: string[];
  summary: string;
};

export type RecordingPreflightResult = {
  ready: boolean;
  errors: string[];
  warnings: string[];
  summary: string;
};

export function runOutputPreflight(destinations: OutputDestination[]): OutputPreflightResult {
  const armedDestinations = destinations.filter((destination) => destination.enabled);
  const errors = armedDestinations.flatMap(validateDestination);
  const warnings = buildWarnings(armedDestinations);

  if (armedDestinations.length === 0) {
    errors.push("Arm at least one destination before streaming.");
  }

  const ready = errors.length === 0;

  return {
    ready,
    destinations: armedDestinations,
    errors,
    warnings,
    summary: ready
      ? `Ready to stream to ${armedDestinations.length} destination${armedDestinations.length === 1 ? "" : "s"}.`
      : errors[0]
  };
}

export function runRecordingPreflight(settings: RecordingSettings): RecordingPreflightResult {
  const errors = validateRecordingSettings(settings);
  const warnings = buildRecordingWarnings(settings);
  const ready = errors.length === 0;

  return {
    ready,
    errors,
    warnings,
    summary: ready
      ? `Ready to record ${settings.format.toUpperCase()} at ${settings.quality} quality with ${settings.isoParticipantIds.length} ISO feed${settings.isoParticipantIds.length === 1 ? "" : "s"}.`
      : errors[0]
  };
}

function validateDestination(destination: OutputDestination) {
  const errors: string[] = [];
  const endpoint = destination.endpoint.trim();
  const streamKey = destination.streamKey?.trim() ?? "";

  if (!endpoint) {
    errors.push(`${destination.name} needs an endpoint.`);
  }

  if ((destination.protocol === "RTMP" || destination.protocol === "WebRTC") && !streamKey) {
    errors.push(`${destination.name} needs a stream key.`);
  }

  if (destination.protocol === "RTMP" && endpoint && !endpoint.startsWith("rtmp://") && !endpoint.startsWith("rtmps://")) {
    errors.push(`${destination.name} endpoint must start with rtmp:// or rtmps://.`);
  }

  if (destination.protocol === "SRT" && endpoint && !endpoint.startsWith("srt://")) {
    errors.push(`${destination.name} endpoint must start with srt://.`);
  }

  if (destination.protocol === "WebRTC" && endpoint && !endpoint.startsWith("https://") && !endpoint.startsWith("wss://")) {
    errors.push(`${destination.name} endpoint must start with https:// or wss://.`);
  }

  return errors;
}

function buildWarnings(destinations: OutputDestination[]) {
  const networkDestinations = destinations.filter((destination) => destination.protocol !== "NDI");

  if (networkDestinations.length > 2) {
    return ["Three or more network destinations may increase encoder and network load."];
  }

  return [];
}

function validateRecordingSettings(settings: RecordingSettings) {
  const errors: string[] = [];
  const folder = settings.folder.trim();
  const filenamePrefix = settings.filenamePrefix.trim();
  const invalidNameCharacters = /[<>:"|?*]/;

  if (!folder) {
    errors.push("Choose a recording folder before recording.");
  }

  if (!filenamePrefix) {
    errors.push("Enter a recording filename before recording.");
  }

  if (invalidNameCharacters.test(filenamePrefix)) {
    errors.push("Recording filename cannot contain < > : \" | ? * characters.");
  }

  if (folder.includes("..")) {
    errors.push("Recording folder cannot include parent-directory traversal.");
  }

  return errors;
}

function buildRecordingWarnings(settings: RecordingSettings) {
  const warnings: string[] = [];

  if (settings.quality === "archive" && settings.format === "mp4") {
    warnings.push("Archive MP4 recordings may take longer to finalize after stopping.");
  }

  if (settings.isoParticipantIds.length > 4) {
    warnings.push("Recording more than 4 ISO feeds may increase disk and encoder load.");
  }

  return warnings;
}
