import type {
  ZoomMediaSpineParticipantPayload,
  ZoomMediaSpineRecordingTargetPayload,
  ZoomMediaSpineSubscriptionRequestPayload,
  ZoomMediaSpineSyncPayload
} from "./zoomMediaSpineSync";
import type { ZoomSdkReadinessInput } from "./zoomSdkReadiness";

export type ZoomMediaSpineServiceJoin = {
  meetingNumber: string;
  displayName: string;
  passcodePresent?: boolean;
};

export type ZoomMediaSpineServiceRequest =
  | {
      id: string;
      type: "zoom-configure";
      config: ZoomSdkReadinessInput;
    }
  | {
      id: string;
      type: "zoom-join";
      join: ZoomMediaSpineServiceJoin;
      participants?: ZoomMediaSpineParticipantPayload[];
    }
  | {
      id: string;
      type: "zoom-roster";
      participants: ZoomMediaSpineParticipantPayload[];
    }
  | {
      id: string;
      type: "zoom-subscriptions";
      subscriptions: ZoomMediaSpineSubscriptionRequestPayload[];
    }
  | {
      id: string;
      type: "zoom-start-recording";
      targets?: ZoomMediaSpineRecordingTargetPayload;
    }
  | {
      id: string;
      type: "zoom-stop-recording";
    }
  | {
      id: string;
      type: "zoom-tick";
      elapsedMs: number;
    }
  | {
      id: string;
      type: "zoom-leave";
    }
  | {
      id: string;
      type: "zoom-snapshot";
    };

export type ZoomMediaSpineServicePlanMode = "join" | "sync" | "leave";

export type ZoomMediaSpineServicePlanOptions = {
  mode?: ZoomMediaSpineServicePlanMode;
  join?: ZoomMediaSpineServiceJoin;
  elapsedMs: number;
  requestIdPrefix?: string;
  includeSnapshot?: boolean;
  previousRecordingActive?: boolean;
};

export type ZoomMediaSpineServicePlan = {
  blocked: boolean;
  requests: ZoomMediaSpineServiceRequest[];
  warnings: string[];
  summary: string;
};

export function buildZoomMediaSpineServicePlan(
  payload: ZoomMediaSpineSyncPayload,
  readinessInput: ZoomSdkReadinessInput,
  options: ZoomMediaSpineServicePlanOptions
): ZoomMediaSpineServicePlan {
  const mode = options.mode ?? "sync";
  const nextId = createRequestIdFactory(options.requestIdPrefix ?? "zoom-spine");
  const requests: ZoomMediaSpineServiceRequest[] = [];

  if (mode === "leave") {
    requests.push({ id: nextId("leave"), type: "zoom-leave" });
    if (options.includeSnapshot) {
      requests.push({ id: nextId("snapshot"), type: "zoom-snapshot" });
    }

    return {
      blocked: false,
      requests,
      warnings: [],
      summary: "Zoom media spine leave request planned."
    };
  }

  requests.push({
    id: nextId("configure"),
    type: "zoom-configure",
    config: readinessInput
  });

  if (mode === "join") {
    if (!options.join) {
      return {
        blocked: true,
        requests,
        warnings: [...payload.warnings, "Zoom join request is missing."],
        summary: "Zoom media spine join blocked because join parameters are missing."
      };
    }

    requests.push({
      id: nextId("join"),
      type: "zoom-join",
      join: options.join,
      participants: payload.participants
    });
  } else {
    requests.push({
      id: nextId("roster"),
      type: "zoom-roster",
      participants: payload.participants
    });
  }

  requests.push({
    id: nextId("subscriptions"),
    type: "zoom-subscriptions",
    subscriptions: payload.subscriptions
  });

  if (payload.recording) {
    requests.push({
      id: nextId("start-recording"),
      type: "zoom-start-recording",
      targets: payload.recording
    });
  } else if (options.previousRecordingActive) {
    requests.push({
      id: nextId("stop-recording"),
      type: "zoom-stop-recording"
    });
  }

  requests.push({
    id: nextId("tick"),
    type: "zoom-tick",
    elapsedMs: options.elapsedMs
  });

  if (options.includeSnapshot) {
    requests.push({
      id: nextId("snapshot"),
      type: "zoom-snapshot"
    });
  }

  return {
    blocked: payload.blocked,
    requests,
    warnings: payload.warnings,
    summary: payload.blocked
      ? `Zoom media spine service plan blocked; ${payload.warnings.length} warning${payload.warnings.length === 1 ? "" : "s"} ${payload.warnings.length === 1 ? "requires" : "require"} attention.`
      : `${requests.length} Zoom media spine service request${requests.length === 1 ? "" : "s"} planned.`
  };
}

function createRequestIdFactory(prefix: string) {
  let sequence = 0;
  return (label: string) => {
    sequence += 1;
    return `${prefix}:${String(sequence).padStart(2, "0")}:${label}`;
  };
}
