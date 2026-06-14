import { RecordingSink } from "./recordingSink.js";
import type {
  MediaCoreFrame,
  MediaCoreProgramFrame,
  MediaCoreRecordingSession,
  MediaCoreRecordingTargets,
  ZoomMediaSpineSyncPayload
} from "./protocol.js";

export type ZoomMediaSpineMeetingState = "idle" | "joining" | "in-meeting" | "leaving" | "error";
export type ZoomMediaSpinePlatform = "macos" | "windows";
export type ZoomMediaSpineMediaKind = "participant-video" | "participant-audio" | "screen-share";
export type ZoomMediaSpineSubscriptionPurpose = "program" | "iso" | "active-speaker" | "screen-share" | "mix";
export type ZoomMediaSpineSubscriptionStatus = "subscribed" | "degraded" | "failed";

export type ZoomMediaSpineRuntimeConfig = {
  platform: ZoomMediaSpinePlatform;
  sdkRuntimePresent: boolean;
  sdkVersion?: string;
  appKeyPresent: boolean;
  oauthConfigured: boolean;
  jwtBrokerConfigured: boolean;
  rawVideoEnabled: boolean;
  rawAudioEnabled: boolean;
  rawShareEnabled: boolean;
};

export type ZoomMediaSpineReadinessReport = {
  status: "ready" | "blocked";
  platform: ZoomMediaSpinePlatform;
  sdkVersion: string;
  blockers: string[];
};

export type ZoomMediaSpineJoinRequest = {
  meetingNumber: string;
  displayName: string;
  passcodePresent?: boolean;
};

export type ZoomMediaSpineParticipant = {
  sdkUserId: string;
  displayName: string;
  role: "host" | "cohost" | "panelist" | "attendee" | "guest";
  videoOn: boolean;
  muted: boolean;
  talking: boolean;
  sharingScreen: boolean;
  audioLevel: number;
  networkQuality: "good" | "low" | "recovering";
};

export type ZoomMediaSpineSubscriptionRequest = {
  participantId: string;
  kind: ZoomMediaSpineMediaKind;
  purpose: ZoomMediaSpineSubscriptionPurpose;
  priority: number;
};

export type ZoomMediaSpineSubscription = ZoomMediaSpineSubscriptionRequest & {
  subscriptionId: string;
  displayName?: string;
  status: ZoomMediaSpineSubscriptionStatus;
  lastResultCode: "ok" | "participant-missing" | "video-off" | "raw-media-disabled" | "low-resolution";
  deliveredWidth?: number;
  deliveredHeight?: number;
  deliveredFps?: number;
  framesReceived: number;
  audioPacketsReceived: number;
  warning?: string;
};

export type ZoomMediaSpineRecordingProof = {
  session?: MediaCoreRecordingSession;
  evidence: {
    programFramesWritten: number;
    isoFramesWritten: number;
    audioPacketsObserved: number;
    subscribedVideoFeeds: number;
  };
};

export type ZoomMediaSpineSnapshot = {
  meetingState: ZoomMediaSpineMeetingState;
  sdkVersion: string;
  participantCount: number;
  activeSpeakerId?: string;
  screenShareParticipantId?: string;
  participants: ZoomMediaSpineParticipant[];
  subscriptions: ZoomMediaSpineSubscription[];
  recording: ZoomMediaSpineRecordingProof;
  warnings: string[];
  events: string[];
};

const DEFAULT_RECORDING_TARGETS: MediaCoreRecordingTargets = {
  targetFolder: "Recordings/CoreVideo Pro/zoom-spine",
  filenamePrefix: "zoom-media-spine",
  format: "mp4",
  quality: "high",
  isoParticipantIds: []
};

export function assessZoomMediaSpineReadiness(config: ZoomMediaSpineRuntimeConfig): ZoomMediaSpineReadinessReport {
  const blockers = [
    config.sdkRuntimePresent ? undefined : "Zoom Meeting SDK runtime is missing.",
    config.appKeyPresent ? undefined : "Meeting SDK app key is missing.",
    config.oauthConfigured ? undefined : "OAuth broker is not configured.",
    config.jwtBrokerConfigured ? undefined : "SDK JWT broker is not configured.",
    config.rawVideoEnabled ? undefined : "Raw participant video is disabled.",
    config.rawAudioEnabled ? undefined : "Raw audio is disabled.",
    config.rawShareEnabled ? undefined : "Raw screen share is disabled."
  ].filter(Boolean) as string[];

  return {
    status: blockers.length ? "blocked" : "ready",
    platform: config.platform,
    sdkVersion: config.sdkVersion ?? "unknown",
    blockers
  };
}

export class ZoomMediaSpineRuntime {
  private meetingState: ZoomMediaSpineMeetingState = "idle";
  private readonly participants = new Map<string, ZoomMediaSpineParticipant>();
  private readonly subscriptions = new Map<string, ZoomMediaSpineSubscription>();
  private readonly recordingSink = new RecordingSink();
  private readonly events: string[] = [];
  private activeSpeakerId?: string;
  private screenShareParticipantId?: string;
  private elapsedMs = 0;

  constructor(private readonly config: ZoomMediaSpineRuntimeConfig) {}

  syncPayload(payload: ZoomMediaSpineSyncPayload, elapsedMs: number): ZoomMediaSpineSnapshot {
    this.elapsedMs = Math.max(0, elapsedMs);
    this.meetingState = payload.blocked ? "error" : "in-meeting";
    this.replaceRoster(payload.participants);
    this.syncSubscriptions(payload.subscriptions);

    if (payload.recording) {
      this.startRecording(payload.recording);
    }

    this.tick(this.elapsedMs);
    this.events.push("Zoom media spine payload accepted by native-core stub.");
    this.events.push(payload.summary);
    return this.snapshot(payload.warnings);
  }

  join(request: ZoomMediaSpineJoinRequest, participants: ZoomMediaSpineParticipant[] = []): ZoomMediaSpineSnapshot {
    const readiness = assessZoomMediaSpineReadiness(this.config);
    this.meetingState = "joining";
    this.events.push(`Joining Zoom meeting ${redactMeetingNumber(request.meetingNumber)} as ${request.displayName}.`);

    if (readiness.status === "blocked") {
      this.meetingState = "error";
      readiness.blockers.forEach((blocker) => this.events.push(`Blocked: ${blocker}`));
      return this.snapshot(readiness.blockers);
    }

    this.meetingState = "in-meeting";
    this.replaceRoster(participants);
    this.events.push(`Joined Zoom meeting with ${participants.length} participant${participants.length === 1 ? "" : "s"}.`);
    return this.snapshot();
  }

  replaceRoster(participants: ZoomMediaSpineParticipant[]): ZoomMediaSpineSnapshot {
    const nextIds = new Set(participants.map((participant) => participant.sdkUserId));
    this.participants.forEach((participant, sdkUserId) => {
      if (!nextIds.has(sdkUserId)) {
        this.events.push(`${participant.displayName} left the meeting.`);
      }
    });

    participants.forEach((participant) => {
      const previous = this.participants.get(participant.sdkUserId);
      this.participants.set(participant.sdkUserId, normalizeParticipant(participant));
      if (!previous) {
        this.events.push(`${participant.displayName} joined the meeting.`);
      } else {
        this.addParticipantTransitionEvents(previous, participant);
      }
    });

    this.activeSpeakerId = participants.find((participant) => participant.talking)?.sdkUserId;
    this.screenShareParticipantId = participants.find((participant) => participant.sharingScreen)?.sdkUserId;
    this.dropInvalidSubscriptions();
    return this.snapshot();
  }

  syncSubscriptions(requests: ZoomMediaSpineSubscriptionRequest[]): ZoomMediaSpineSnapshot {
    const desired = dedupeRequests(requests);
    this.subscriptions.clear();
    desired.forEach((request) => {
      const subscription = this.buildSubscription(request);
      this.subscriptions.set(subscription.subscriptionId, subscription);
      if (subscription.status === "failed") {
        this.events.push(`Subscription failed: ${subscription.warning}`);
      }
    });

    return this.snapshot();
  }

  startRecording(targets: Partial<MediaCoreRecordingTargets> = {}): ZoomMediaSpineSnapshot {
    this.recordingSink.start({ ...DEFAULT_RECORDING_TARGETS, ...targets }, this.elapsedMs, `zoom-spine-${this.elapsedMs}`);
    this.events.push("Recording proof started from Zoom media spine.");
    return this.snapshot();
  }

  tick(elapsedMs: number): ZoomMediaSpineSnapshot {
    this.elapsedMs = Math.max(0, elapsedMs);
    const frameSubscriptions = [...this.subscriptions.values()].filter(isFrameSubscription);
    const audioSubscriptions = [...this.subscriptions.values()].filter(
      (subscription) => subscription.status !== "failed" && subscription.kind === "participant-audio"
    );

    frameSubscriptions.forEach((subscription) => {
      subscription.framesReceived += 1;
    });
    audioSubscriptions.forEach((subscription) => {
      subscription.audioPacketsReceived += 1;
    });

    const frames = frameSubscriptions.map((subscription): MediaCoreFrame => {
      const participant = this.participants.get(subscription.participantId);
      return {
        sourceId: `${subscription.kind}:${subscription.participantId}`,
        participantId: subscription.participantId,
        kind: subscription.kind,
        frameNumber: subscription.framesReceived,
        timestampMs: this.elapsedMs,
        width: subscription.deliveredWidth ?? 1280,
        height: subscription.deliveredHeight ?? 720,
        fps: subscription.deliveredFps ?? 30,
        health: participant?.networkQuality === "low" ? "low-resolution" : "live"
      };
    });

    this.recordingSink.writeFrames(frames, this.elapsedMs, buildProgramFrame(frames, this.elapsedMs));
    return this.snapshot();
  }

  stopRecording(): ZoomMediaSpineSnapshot {
    this.recordingSink.stop(this.elapsedMs);
    this.events.push("Recording proof stopped from Zoom media spine.");
    return this.snapshot();
  }

  leave(): ZoomMediaSpineSnapshot {
    this.meetingState = "leaving";
    this.participants.clear();
    this.subscriptions.clear();
    this.activeSpeakerId = undefined;
    this.screenShareParticipantId = undefined;
    this.recordingSink.stop(this.elapsedMs);
    this.meetingState = "idle";
    this.events.push("Left Zoom meeting and cleared raw media subscriptions.");
    return this.snapshot();
  }

  snapshot(extraWarnings: string[] = []): ZoomMediaSpineSnapshot {
    const subscriptions = [...this.subscriptions.values()];
    const recording = this.recordingSink.snapshot();
    const warnings = [
      ...extraWarnings,
      ...subscriptions.map((subscription) => subscription.warning).filter(Boolean),
      recording?.warning
    ].filter(Boolean) as string[];

    return {
      meetingState: this.meetingState,
      sdkVersion: this.config.sdkVersion ?? "unknown",
      participantCount: this.participants.size,
      activeSpeakerId: this.activeSpeakerId,
      screenShareParticipantId: this.screenShareParticipantId,
      participants: [...this.participants.values()].map((participant) => ({ ...participant })),
      subscriptions: subscriptions.map((subscription) => ({ ...subscription })),
      recording: {
        session: recording,
        evidence: {
          programFramesWritten: recording?.streams.find((stream) => stream.kind === "program")?.framesWritten ?? 0,
          isoFramesWritten:
            recording?.streams.filter((stream) => stream.kind === "iso").reduce((total, stream) => total + stream.framesWritten, 0) ?? 0,
          audioPacketsObserved: subscriptions.reduce((total, subscription) => total + subscription.audioPacketsReceived, 0),
          subscribedVideoFeeds: subscriptions.filter((subscription) => subscription.status !== "failed" && subscription.kind === "participant-video").length
        }
      },
      warnings: [...new Set(warnings)],
      events: [...this.events]
    };
  }

  private buildSubscription(request: ZoomMediaSpineSubscriptionRequest): ZoomMediaSpineSubscription {
    const participant = this.participants.get(request.participantId);
    const subscriptionId = `${request.kind}:${request.participantId}:${request.purpose}`;
    if (!participant) {
      return failedSubscription(request, subscriptionId, "participant-missing", `${request.participantId} is not in the Zoom SDK roster.`);
    }

    if (!rawMediaEnabled(this.config, request.kind)) {
      return failedSubscription(request, subscriptionId, "raw-media-disabled", `${request.kind} callbacks are not enabled in the Zoom SDK helper.`);
    }

    if ((request.kind === "participant-video" || request.kind === "screen-share") && !participant.videoOn) {
      return failedSubscription(request, subscriptionId, "video-off", `${participant.displayName} video is off.`);
    }

    if (request.kind === "screen-share" && !participant.sharingScreen) {
      return failedSubscription(request, subscriptionId, "participant-missing", `${participant.displayName} is not sharing their screen.`);
    }

    const degraded = participant.networkQuality === "low";
    return {
      ...request,
      subscriptionId,
      displayName: participant.displayName,
      status: degraded ? "degraded" : "subscribed",
      lastResultCode: degraded ? "low-resolution" : "ok",
      deliveredWidth: request.kind === "screen-share" ? 1920 : degraded ? 640 : 1280,
      deliveredHeight: request.kind === "screen-share" ? 1080 : degraded ? 360 : 720,
      deliveredFps: request.kind === "screen-share" ? 30 : degraded ? 15 : 30,
      framesReceived: 0,
      audioPacketsReceived: 0,
      warning: degraded ? `${participant.displayName} raw media subscribed below target resolution.` : undefined
    };
  }

  private dropInvalidSubscriptions() {
    [...this.subscriptions.values()].forEach((subscription) => {
      if (!this.participants.has(subscription.participantId)) {
        this.subscriptions.delete(subscription.subscriptionId);
      }
    });
  }

  private addParticipantTransitionEvents(previous: ZoomMediaSpineParticipant, current: ZoomMediaSpineParticipant) {
    if (previous.videoOn && !current.videoOn) {
      this.events.push(`${current.displayName} camera turned off.`);
    } else if (!previous.videoOn && current.videoOn) {
      this.events.push(`${current.displayName} camera restored.`);
    }

    if (!previous.sharingScreen && current.sharingScreen) {
      this.events.push(`${current.displayName} started screen sharing.`);
    } else if (previous.sharingScreen && !current.sharingScreen) {
      this.events.push(`${current.displayName} stopped screen sharing.`);
    }

    if (!previous.talking && current.talking) {
      this.events.push(`${current.displayName} became active speaker.`);
    }
  }
}

function normalizeParticipant(participant: ZoomMediaSpineParticipant): ZoomMediaSpineParticipant {
  return {
    ...participant,
    sdkUserId: String(participant.sdkUserId),
    displayName: participant.displayName.trim() || `Zoom User ${participant.sdkUserId}`,
    audioLevel: Math.max(0, Math.min(100, participant.audioLevel))
  };
}

function dedupeRequests(requests: ZoomMediaSpineSubscriptionRequest[]) {
  const byKindAndParticipant = new Map<string, ZoomMediaSpineSubscriptionRequest>();
  [...requests].sort((left, right) => left.priority - right.priority).forEach((request) => {
    const key = `${request.kind}:${request.participantId}`;
    if (!byKindAndParticipant.has(key)) {
      byKindAndParticipant.set(key, request);
    }
  });

  return [...byKindAndParticipant.values()];
}

function rawMediaEnabled(config: ZoomMediaSpineRuntimeConfig, kind: ZoomMediaSpineMediaKind) {
  if (kind === "participant-audio") {
    return config.rawAudioEnabled;
  }

  if (kind === "screen-share") {
    return config.rawShareEnabled;
  }

  return config.rawVideoEnabled;
}

function isFrameSubscription(
  subscription: ZoomMediaSpineSubscription
): subscription is ZoomMediaSpineSubscription & { kind: "participant-video" | "screen-share" } {
  return subscription.status !== "failed" && (subscription.kind === "participant-video" || subscription.kind === "screen-share");
}

function failedSubscription(
  request: ZoomMediaSpineSubscriptionRequest,
  subscriptionId: string,
  lastResultCode: ZoomMediaSpineSubscription["lastResultCode"],
  warning: string
): ZoomMediaSpineSubscription {
  return {
    ...request,
    subscriptionId,
    status: "failed",
    lastResultCode,
    framesReceived: 0,
    audioPacketsReceived: 0,
    warning
  };
}

function buildProgramFrame(frames: MediaCoreFrame[], elapsedMs: number): MediaCoreProgramFrame | undefined {
  if (frames.length === 0) {
    return undefined;
  }

  return {
    frameNumber: Math.max(...frames.map((frame) => frame.frameNumber)),
    timestampMs: elapsedMs,
    renderPlanId: "zoom-media-spine-proof",
    width: 1920,
    height: 1080,
    fps: 30,
    layerCount: frames.length,
    colorGrade: {
      lut: "none",
      exposure: 0,
      contrast: 0,
      saturation: 0,
      temperature: 0
    },
    health: frames.some((frame) => frame.health === "low-resolution") ? "degraded" : "live",
    warning: frames.some((frame) => frame.health === "low-resolution") ? "Program includes a low-resolution Zoom feed." : undefined
  };
}

function redactMeetingNumber(meetingNumber: string) {
  if (meetingNumber.length <= 4) {
    return "redacted";
  }

  return `${"*".repeat(Math.max(0, meetingNumber.length - 4))}${meetingNumber.slice(-4)}`;
}
