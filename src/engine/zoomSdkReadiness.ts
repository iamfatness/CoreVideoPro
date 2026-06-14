import type { Participant, ProductionState, SceneTemplate } from "../domain/production";

export type ZoomSdkRuntimePlatform = "macos" | "windows";

export type ZoomSdkReadinessInput = {
  platform: ZoomSdkRuntimePlatform;
  sdkRuntimePresent: boolean;
  sdkVersion?: string;
  appKeyPresent: boolean;
  oauthConfigured: boolean;
  jwtBrokerConfigured: boolean;
  rawVideoEnabled: boolean;
  rawAudioEnabled: boolean;
  rawShareEnabled: boolean;
  packagingPath?: string;
};

export type ZoomSdkReadinessStatus = "ready" | "warning" | "blocked";

export type ZoomSdkReadinessCheck = {
  id: string;
  status: ZoomSdkReadinessStatus;
  label: string;
  detail: string;
};

export type ZoomSdkReadinessReport = {
  status: ZoomSdkReadinessStatus;
  platform: ZoomSdkRuntimePlatform;
  sdkVersion: string;
  checks: ZoomSdkReadinessCheck[];
  blockers: string[];
  warnings: string[];
  summary: string;
};

export type ZoomRawMediaKind = "participant-video" | "participant-audio" | "screen-share";
export type ZoomRawMediaPurpose = "program" | "iso" | "active-speaker" | "screen-share" | "mix";

export type ZoomRawMediaSubscription = {
  subscriptionId: string;
  participantId: string;
  displayName: string;
  kind: ZoomRawMediaKind;
  purpose: ZoomRawMediaPurpose;
  priority: number;
};

export type ZoomRawMediaSubscriptionPlan = {
  subscriptions: ZoomRawMediaSubscription[];
  videoSubscriptionCount: number;
  audioSubscriptionCount: number;
  screenShareSubscriptionCount: number;
  warnings: string[];
  blocked: boolean;
  summary: string;
};

export type ZoomRawMediaSubscriptionOptions = {
  maxVideoSubscriptions?: number;
  selectedBreakoutRoomId?: string;
};

const DEFAULT_MAX_VIDEO_SUBSCRIPTIONS = 8;

export function assessZoomSdkReadiness(input: ZoomSdkReadinessInput): ZoomSdkReadinessReport {
  const checks: ZoomSdkReadinessCheck[] = [
    buildCheck(
      "sdk-runtime",
      input.sdkRuntimePresent,
      "Zoom Meeting SDK runtime",
      input.sdkRuntimePresent
        ? `Runtime found${input.packagingPath ? ` at ${input.packagingPath}` : ""}.`
        : "Zoom Meeting SDK runtime is missing from the packaged helper process."
    ),
    buildCheck(
      "app-key",
      input.appKeyPresent,
      "Meeting SDK app key",
      input.appKeyPresent ? "Meeting SDK app key is configured." : "Meeting SDK app key is missing."
    ),
    buildCheck(
      "oauth",
      input.oauthConfigured,
      "OAuth broker",
      input.oauthConfigured ? "OAuth broker is configured for meeting authorization." : "OAuth broker is not configured."
    ),
    buildCheck(
      "jwt-broker",
      input.jwtBrokerConfigured,
      "SDK JWT broker",
      input.jwtBrokerConfigured ? "SDK JWT broker is configured." : "SDK JWT broker is not configured."
    ),
    buildCheck(
      "raw-video",
      input.rawVideoEnabled,
      "Raw participant video",
      input.rawVideoEnabled ? "Raw participant video callbacks are enabled." : "Raw participant video callbacks are disabled."
    ),
    buildCheck(
      "raw-audio",
      input.rawAudioEnabled,
      "Raw mixed and isolated audio",
      input.rawAudioEnabled ? "Raw audio callbacks are enabled." : "Raw audio callbacks are disabled."
    ),
    buildCheck(
      "raw-share",
      input.rawShareEnabled,
      "Raw screen share",
      input.rawShareEnabled ? "Raw screen-share callbacks are enabled." : "Raw screen-share callbacks are disabled."
    )
  ];

  const blockers = checks.filter((check) => check.status === "blocked").map((check) => check.detail);
  const warnings = input.sdkVersion ? [] : ["SDK version is unknown; include it in support bundles before beta."];
  const status: ZoomSdkReadinessStatus = blockers.length ? "blocked" : warnings.length ? "warning" : "ready";

  return {
    status,
    platform: input.platform,
    sdkVersion: input.sdkVersion ?? "unknown",
    checks,
    blockers,
    warnings,
    summary:
      status === "ready"
        ? `Zoom SDK media path ready on ${input.platform}.`
        : status === "warning"
          ? `Zoom SDK media path usable on ${input.platform} with ${warnings.length} warning.`
          : `Zoom SDK media path blocked by ${blockers.length} missing requirement${blockers.length === 1 ? "" : "s"}.`
  };
}

export function buildZoomRawMediaSubscriptionPlan(
  state: ProductionState,
  options: ZoomRawMediaSubscriptionOptions = {}
): ZoomRawMediaSubscriptionPlan {
  const maxVideoSubscriptions = options.maxVideoSubscriptions ?? DEFAULT_MAX_VIDEO_SUBSCRIPTIONS;
  const selectedBreakoutRoomId = options.selectedBreakoutRoomId ?? state.selectedBreakoutRoomId;
  const participants = filterParticipantsForRoom(state.participants, selectedBreakoutRoomId);
  const participantById = new Map(participants.map((participant) => [participant.id, participant]));
  const activeScene = state.scenes.find((scene) => scene.id === state.activeSceneId) ?? state.scenes[0];
  const videoRequests = collectVideoRequests(activeScene, participants, state.recordingSettings.isoParticipantIds);
  const warnings: string[] = [];
  const subscriptions = new Map<string, ZoomRawMediaSubscription>();

  videoRequests
    .sort((left, right) => left.priority - right.priority)
    .forEach((request) => {
      const participant = participantById.get(request.participantId);
      if (!participant) {
        warnings.push(`Participant ${request.participantId} is not available for ${request.purpose} video.`);
        return;
      }

      if (participant.health === "video-off") {
        warnings.push(`${participant.name} cannot provide ${request.purpose} video because camera is off.`);
        return;
      }

      addSubscription(subscriptions, {
        subscriptionId: `${request.kind}:${participant.id}:${request.purpose}`,
        participantId: participant.id,
        displayName: participant.name,
        kind: request.kind,
        purpose: request.purpose,
        priority: request.priority
      });
    });

  participants.forEach((participant, index) => {
    addSubscription(subscriptions, {
      subscriptionId: `participant-audio:${participant.id}:mix`,
      participantId: participant.id,
      displayName: participant.name,
      kind: "participant-audio",
      purpose: "mix",
      priority: 40 + index
    });
  });

  const ordered = [...subscriptions.values()].sort((left, right) => left.priority - right.priority || left.displayName.localeCompare(right.displayName));
  const videoSubscriptions = ordered.filter((subscription) => subscription.kind === "participant-video");
  const overflow = videoSubscriptions.slice(maxVideoSubscriptions);
  overflow.forEach((subscription) => {
    subscriptions.delete(subscription.subscriptionId);
    warnings.push(`${subscription.displayName} ${subscription.purpose} video not subscribed; max video subscription limit is ${maxVideoSubscriptions}.`);
  });

  const finalSubscriptions = [...subscriptions.values()].sort(
    (left, right) => left.priority - right.priority || left.displayName.localeCompare(right.displayName)
  );
  const videoSubscriptionCount = finalSubscriptions.filter((subscription) => subscription.kind === "participant-video").length;
  const audioSubscriptionCount = finalSubscriptions.filter((subscription) => subscription.kind === "participant-audio").length;
  const screenShareSubscriptionCount = finalSubscriptions.filter((subscription) => subscription.kind === "screen-share").length;

  return {
    subscriptions: finalSubscriptions,
    videoSubscriptionCount,
    audioSubscriptionCount,
    screenShareSubscriptionCount,
    warnings,
    blocked: videoSubscriptionCount === 0 && screenShareSubscriptionCount === 0,
    summary: `${videoSubscriptionCount} video, ${screenShareSubscriptionCount} screen-share, ${audioSubscriptionCount} audio raw Zoom subscriptions planned.`
  };
}

function buildCheck(id: string, passed: boolean, label: string, detail: string): ZoomSdkReadinessCheck {
  return {
    id,
    status: passed ? "ready" : "blocked",
    label,
    detail
  };
}

function filterParticipantsForRoom(participants: Participant[], selectedBreakoutRoomId: string) {
  if (selectedBreakoutRoomId === "all") {
    return participants;
  }

  return participants.filter((participant) => participant.breakoutRoomId === selectedBreakoutRoomId);
}

function collectVideoRequests(scene: SceneTemplate, participants: Participant[], isoParticipantIds: string[]) {
  const activeSpeaker = participants.find((participant) => participant.isActiveSpeaker);
  const screenShare = participants.find((participant) => participant.isScreenSharing);
  const requests: Array<{
    participantId: string;
    kind: Exclude<ZoomRawMediaKind, "participant-audio">;
    purpose: Exclude<ZoomRawMediaPurpose, "mix">;
    priority: number;
  }> = [];

  (scene.routes ?? []).forEach((route, index) => {
    if ((route.mode === "fixed" || route.mode === "spotlight") && route.participantId) {
      requests.push({ participantId: route.participantId, kind: "participant-video", purpose: "program", priority: 10 + index });
    }

    if (route.mode === "active-speaker" && activeSpeaker) {
      requests.push({ participantId: activeSpeaker.id, kind: "participant-video", purpose: "active-speaker", priority: 12 + index });
    }

    if (route.mode === "screen-share" && screenShare) {
      requests.push({ participantId: screenShare.id, kind: "screen-share", purpose: "screen-share", priority: 8 + index });
    }
  });

  if (!scene.routes?.length) {
    inferVideoRequestsFromLayout(scene, participants).forEach((request) => requests.push(request));
  }

  isoParticipantIds.forEach((participantId, index) => {
    requests.push({ participantId, kind: "participant-video", purpose: "iso", priority: 30 + index });
  });

  return requests;
}

function inferVideoRequestsFromLayout(scene: SceneTemplate, participants: Participant[]) {
  const activeSpeaker = participants.find((participant) => participant.isActiveSpeaker) ?? participants[0];
  const screenShare = participants.find((participant) => participant.isScreenSharing);
  const visibleParticipants = participants.filter((participant) => participant.health !== "video-off");

  if (scene.layout === "speaker-slides" && screenShare) {
    return [
      { participantId: activeSpeaker.id, kind: "participant-video" as const, purpose: "active-speaker" as const, priority: 10 },
      { participantId: screenShare.id, kind: "screen-share" as const, purpose: "screen-share" as const, priority: 8 }
    ];
  }

  if (scene.layout === "two-up") {
    return visibleParticipants.slice(0, 2).map((participant, index) => ({
      participantId: participant.id,
      kind: "participant-video" as const,
      purpose: "program" as const,
      priority: 10 + index
    }));
  }

  if (scene.layout === "smart-grid") {
    return visibleParticipants.slice(0, 6).map((participant, index) => ({
      participantId: participant.id,
      kind: "participant-video" as const,
      purpose: "program" as const,
      priority: 10 + index
    }));
  }

  return activeSpeaker
    ? [{ participantId: activeSpeaker.id, kind: "participant-video" as const, purpose: "active-speaker" as const, priority: 10 }]
    : [];
}

function addSubscription(subscriptions: Map<string, ZoomRawMediaSubscription>, subscription: ZoomRawMediaSubscription) {
  const existing = [...subscriptions.values()].find(
    (candidate) => candidate.participantId === subscription.participantId && candidate.kind === subscription.kind
  );

  if (existing) {
    if (subscription.priority < existing.priority) {
      subscriptions.delete(existing.subscriptionId);
      subscriptions.set(subscription.subscriptionId, subscription);
    }
    return;
  }

  subscriptions.set(subscription.subscriptionId, subscription);
}
