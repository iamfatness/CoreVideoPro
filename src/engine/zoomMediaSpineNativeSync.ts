import type { ProductionState } from "../domain/production";
import type { NativeHostBridge } from "./nativeHostBridge";
import {
  buildZoomMediaSpineSyncPayload,
  type ZoomMediaSpineParticipantPayload,
  type ZoomMediaSpineSubscriptionRequestPayload,
  type ZoomMediaSpineSyncOptions,
  type ZoomMediaSpineSyncPayload
} from "./zoomMediaSpineSync";
import type { ZoomSdkReadinessInput } from "./zoomSdkReadiness";

export type ZoomMediaSpineSubscriptionStatus = "subscribed" | "degraded" | "failed";
export type ZoomMediaSpineSubscriptionResultCode =
  | "ok"
  | "participant-missing"
  | "video-off"
  | "raw-media-disabled"
  | "raw-audio-unavailable"
  | "low-resolution";

export type ZoomMediaSpineRawAudioEvidence = {
  participantId: string;
  displayName?: string;
  status: ZoomMediaSpineSubscriptionStatus;
  lastResultCode: ZoomMediaSpineSubscriptionResultCode;
  audioPacketsReceived: number;
  warning?: string;
};

export type ZoomMediaSpineNativeSubscription = ZoomMediaSpineSubscriptionRequestPayload & {
  subscriptionId: string;
  displayName?: string;
  status: ZoomMediaSpineSubscriptionStatus;
  lastResultCode: ZoomMediaSpineSubscriptionResultCode;
  deliveredWidth?: number;
  deliveredHeight?: number;
  deliveredFps?: number;
  framesReceived: number;
  audioPacketsReceived: number;
  warning?: string;
};

export type ZoomMediaSpineNativeSnapshot = {
  meetingState: "idle" | "joining" | "in-meeting" | "leaving" | "error";
  sdkVersion: string;
  participantCount: number;
  activeSpeakerId?: string;
  screenShareParticipantId?: string;
  participants: ZoomMediaSpineParticipantPayload[];
  subscriptions: ZoomMediaSpineNativeSubscription[];
  recording: {
    session?: {
      sessionId: string;
      active: boolean;
      status: "recording" | "warning" | "stopped" | "failed";
      targetFolder: string;
      filenamePrefix: string;
      format: string;
      quality: string;
    };
    evidence: {
      programFramesWritten: number;
      isoFramesWritten: number;
      audioPacketsObserved: number;
      subscribedVideoFeeds: number;
      rawAudioByParticipant?: ZoomMediaSpineRawAudioEvidence[];
    };
  };
  warnings: string[];
  events: string[];
};

export interface ZoomMediaSpineNativeSyncEngine {
  syncProduction(
    state: ProductionState,
    readinessInput: ZoomSdkReadinessInput,
    elapsedMs: number,
    options?: ZoomMediaSpineSyncOptions
  ): Promise<ZoomMediaSpineNativeSnapshot>;
}

export class NativeHostZoomMediaSpineSyncEngine implements ZoomMediaSpineNativeSyncEngine {
  constructor(private readonly bridge: NativeHostBridge) {}

  async syncProduction(
    state: ProductionState,
    readinessInput: ZoomSdkReadinessInput,
    elapsedMs: number,
    options: ZoomMediaSpineSyncOptions = {}
  ) {
    const payload = buildZoomMediaSpineSyncPayload(state, readinessInput, options);
    if (!this.bridge.syncZoomMediaSpine) {
      return buildFallbackZoomMediaSpineSnapshot(payload, elapsedMs, "Native host has no Zoom media spine bridge; using renderer-side sync proof.");
    }

    return this.bridge.syncZoomMediaSpine(payload, elapsedMs);
  }
}

export function buildFallbackZoomMediaSpineSnapshot(
  payload: ZoomMediaSpineSyncPayload,
  elapsedMs: number,
  bridgeWarning?: string
): ZoomMediaSpineNativeSnapshot {
  const participantsById = new Map(payload.participants.map((participant) => [participant.sdkUserId, participant]));
  const subscriptions = payload.subscriptions.map((subscription): ZoomMediaSpineNativeSubscription => {
    const participant = participantsById.get(subscription.participantId);
    const rawMediaDisabled = rawMediaDisabledResultFor(payload, subscription.kind);
    const videoOffWarning =
      participant && (subscription.kind === "participant-video" || subscription.kind === "screen-share") && !participant.videoOn
        ? `${participant.displayName} video is off.`
        : undefined;
    const lowResolution = participant?.networkQuality === "low";
    const failedWarning = rawMediaDisabled?.warning ?? videoOffWarning;

    return {
      ...subscription,
      subscriptionId: `${subscription.kind}:${subscription.participantId}:${subscription.purpose}`,
      displayName: participant?.displayName,
      status: failedWarning ? "failed" : lowResolution ? "degraded" : "subscribed",
      lastResultCode: failedWarning ? (rawMediaDisabled?.code ?? "video-off") : lowResolution ? "low-resolution" : "ok",
      deliveredWidth: subscription.kind === "screen-share" ? 1920 : lowResolution ? 640 : 1280,
      deliveredHeight: subscription.kind === "screen-share" ? 1080 : lowResolution ? 360 : 720,
      deliveredFps: subscription.kind === "screen-share" ? 30 : lowResolution ? 15 : 30,
      framesReceived: subscription.kind === "participant-audio" || failedWarning ? 0 : Math.max(1, Math.floor(elapsedMs / 33)),
      audioPacketsReceived: subscription.kind === "participant-audio" && !failedWarning ? Math.max(1, Math.floor(elapsedMs / 20)) : 0,
      warning: failedWarning ?? (lowResolution && participant ? `${participant.displayName} raw media subscribed below target resolution.` : undefined)
    };
  });
  const activeSpeaker = payload.participants.find((participant) => participant.talking);
  const screenShare = payload.participants.find((participant) => participant.sharingScreen);
  const subscribedVideoFeeds = subscriptions.filter((subscription) => subscription.status !== "failed" && subscription.kind === "participant-video").length;
  const rawAudioByParticipant = subscriptions
    .filter((subscription) => subscription.kind === "participant-audio")
    .map((subscription): ZoomMediaSpineRawAudioEvidence => ({
      participantId: subscription.participantId,
      displayName: subscription.displayName,
      status: subscription.status,
      lastResultCode: subscription.lastResultCode,
      audioPacketsReceived: subscription.audioPacketsReceived,
      warning: subscription.warning
    }));
  const recordingActive = Boolean(payload.recording);

  return {
    meetingState: payload.blocked ? "error" : "in-meeting",
    sdkVersion: payload.readiness.sdkVersion,
    participantCount: payload.participants.length,
    activeSpeakerId: activeSpeaker?.sdkUserId,
    screenShareParticipantId: screenShare?.sdkUserId,
    participants: payload.participants,
    subscriptions,
    recording: {
      session: payload.recording
        ? {
            sessionId: `renderer-zoom-spine-${elapsedMs}`,
            active: true,
            status: payload.warnings.length ? "warning" : "recording",
            targetFolder: payload.recording.targetFolder,
            filenamePrefix: payload.recording.filenamePrefix,
            format: payload.recording.format,
            quality: payload.recording.quality
          }
        : undefined,
      evidence: {
        programFramesWritten: recordingActive && subscribedVideoFeeds > 0 ? Math.max(1, Math.floor(elapsedMs / 33)) : 0,
        isoFramesWritten: recordingActive ? Math.max(0, payload.recording?.isoParticipantIds.length ?? 0) * Math.max(1, Math.floor(elapsedMs / 33)) : 0,
        audioPacketsObserved: subscriptions.reduce((total, subscription) => total + subscription.audioPacketsReceived, 0),
        subscribedVideoFeeds,
        rawAudioByParticipant
      }
    },
    warnings: [...new Set([...payload.warnings, bridgeWarning].filter(Boolean) as string[])],
    events: [
      bridgeWarning ?? "Zoom media spine payload accepted by native host bridge.",
      payload.summary
    ]
  };
}

function rawMediaDisabledResultFor(
  payload: ZoomMediaSpineSyncPayload,
  kind: ZoomMediaSpineSubscriptionRequestPayload["kind"]
): { code: ZoomMediaSpineSubscriptionResultCode; warning: string } | undefined {
  if (kind === "participant-video" && payload.readiness.checks.some((check) => check.id === "raw-video" && check.status === "blocked")) {
    return { code: "raw-media-disabled", warning: "participant-video callbacks are not enabled in the Zoom SDK helper." };
  }

  if (kind === "participant-audio" && payload.readiness.checks.some((check) => check.id === "raw-audio" && check.status === "blocked")) {
    return {
      code: "raw-audio-unavailable",
      warning:
        "Raw mixed and isolated audio callbacks are disabled; enable raw audio in the Zoom SDK helper before live audio validation."
    };
  }

  if (kind === "screen-share" && payload.readiness.checks.some((check) => check.id === "raw-share" && check.status === "blocked")) {
    return { code: "raw-media-disabled", warning: "screen-share callbacks are not enabled in the Zoom SDK helper." };
  }

  return undefined;
}
