import type { MediaCoreAudioRoutingBus, MediaCoreAudioRoutingMatrix, MediaCoreAudioRoutingSend } from "./protocol.js";

export const AUDIO_ROUTING_BUSES: MediaCoreAudioRoutingBus[] = ["pgm-l", "pgm-r", "iso-1", "iso-2", "mon", "stream"];

export const MIN_AUDIO_ROUTING_GAIN_DB = -60;
export const MAX_AUDIO_ROUTING_GAIN_DB = 10;

const IDLE_MATRIX: MediaCoreAudioRoutingMatrix = {
  status: "idle",
  routedSendCount: 0,
  routedSourceCount: 0,
  busSourceCounts: AUDIO_ROUTING_BUSES.map((busId) => ({ busId, sourceCount: 0 })),
  sends: [],
  summary: "Audio routing matrix idle.",
  warnings: []
};

function isAudioRoutingBus(value: string): value is MediaCoreAudioRoutingBus {
  return (AUDIO_ROUTING_BUSES as string[]).includes(value);
}

/**
 * Stateful model for the audio routing gain matrix. Validates incoming sends,
 * clamps gain to the supported range, and summarizes the routed crosspoints so
 * the snapshot can drive the audio mix and operator readouts.
 */
export class AudioRoutingMatrixModel {
  private sends: MediaCoreAudioRoutingSend[] = [];
  private syncWarnings: string[] = [];
  private hasSynced = false;

  sync(sends: MediaCoreAudioRoutingSend[]) {
    this.hasSynced = true;
    const warnings: string[] = [];
    const normalized: MediaCoreAudioRoutingSend[] = [];
    const seen = new Set<string>();
    const routedSourceIds = new Set<string>();

    sends.forEach((send) => {
      const sourceId = typeof send.sourceId === "string" ? send.sourceId.trim() : "";
      if (!sourceId) {
        warnings.push("Audio routing send is missing a sourceId.");
        return;
      }

      if (!isAudioRoutingBus(send.busId)) {
        warnings.push(`Audio routing send for ${sourceId} targets unknown bus ${String(send.busId)}.`);
        return;
      }

      const key = `${sourceId}:${send.busId}`;
      if (seen.has(key)) {
        warnings.push(`Audio routing send ${sourceId} → ${send.busId} is duplicated; keeping the first value.`);
        return;
      }
      seen.add(key);

      const rawGain = Number.isFinite(send.gainDb) ? send.gainDb : 0;
      if (rawGain < MIN_AUDIO_ROUTING_GAIN_DB || rawGain > MAX_AUDIO_ROUTING_GAIN_DB) {
        warnings.push(
          `Audio routing gain ${rawGain} dB for ${sourceId} → ${send.busId} is outside [${MIN_AUDIO_ROUTING_GAIN_DB}, ${MAX_AUDIO_ROUTING_GAIN_DB}] dB; clamped.`
        );
      }

      const gainDb = Math.min(MAX_AUDIO_ROUTING_GAIN_DB, Math.max(MIN_AUDIO_ROUTING_GAIN_DB, rawGain));
      routedSourceIds.add(sourceId);
      normalized.push({ sourceId, busId: send.busId, gainDb });
    });

    // Surface sources that appear in the payload but never reach a bus.
    const requestedSourceIds = new Set(
      sends.map((send) => (typeof send.sourceId === "string" ? send.sourceId.trim() : "")).filter(Boolean)
    );
    requestedSourceIds.forEach((sourceId) => {
      if (!routedSourceIds.has(sourceId)) {
        warnings.push(`Audio routing source ${sourceId} is routed to no bus.`);
      }
    });

    this.sends = normalized;
    this.syncWarnings = [...new Set(warnings)];
    return this.snapshot();
  }

  snapshot(): MediaCoreAudioRoutingMatrix {
    if (!this.hasSynced) {
      return {
        ...IDLE_MATRIX,
        busSourceCounts: IDLE_MATRIX.busSourceCounts.map((entry) => ({ ...entry })),
        sends: [],
        warnings: []
      };
    }

    const busSources = new Map<MediaCoreAudioRoutingBus, Set<string>>(AUDIO_ROUTING_BUSES.map((busId) => [busId, new Set<string>()]));
    const routedSourceIds = new Set<string>();
    this.sends.forEach((send) => {
      busSources.get(send.busId)?.add(send.sourceId);
      routedSourceIds.add(send.sourceId);
    });

    const busSourceCounts = AUDIO_ROUTING_BUSES.map((busId) => ({
      busId,
      sourceCount: busSources.get(busId)?.size ?? 0
    }));

    const summary =
      this.sends.length === 0
        ? "No audio crosspoints routed."
        : `${this.sends.length} send${this.sends.length === 1 ? "" : "s"} from ${routedSourceIds.size} source${
            routedSourceIds.size === 1 ? "" : "s"
          } across ${busSourceCounts.filter((entry) => entry.sourceCount > 0).length} bus(es).`;

    return {
      status: this.syncWarnings.length > 0 ? "warning" : this.sends.length > 0 ? "live" : "idle",
      routedSendCount: this.sends.length,
      routedSourceCount: routedSourceIds.size,
      busSourceCounts,
      sends: this.sends.map((send) => ({ ...send })),
      summary,
      warnings: [...this.syncWarnings]
    };
  }
}
