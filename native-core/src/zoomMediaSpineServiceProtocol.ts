import type {
  ZoomMediaSpineJoinRequest,
  ZoomMediaSpineParticipant,
  ZoomMediaSpineRuntimeConfig,
  ZoomMediaSpineSnapshot,
  ZoomMediaSpineSubscriptionRequest
} from "./zoomMediaSpine.js";
import type { MediaCoreRecordingTargets } from "./protocol.js";

export type ZoomMediaSpineServiceRequest =
  | {
      id: string;
      type: "zoom-configure";
      config: ZoomMediaSpineRuntimeConfig;
    }
  | {
      id: string;
      type: "zoom-join";
      join: ZoomMediaSpineJoinRequest;
      participants?: ZoomMediaSpineParticipant[];
    }
  | {
      id: string;
      type: "zoom-roster";
      participants: ZoomMediaSpineParticipant[];
    }
  | {
      id: string;
      type: "zoom-subscriptions";
      subscriptions: ZoomMediaSpineSubscriptionRequest[];
    }
  | {
      id: string;
      type: "zoom-start-recording";
      targets?: Partial<MediaCoreRecordingTargets>;
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

export type ZoomMediaSpineServiceResponse =
  | {
      id: string;
      ok: true;
      zoom: ZoomMediaSpineSnapshot;
    }
  | {
      id: string;
      ok: false;
      error: {
        code: "invalid-request" | "zoom-runtime-unconfigured";
        message: string;
      };
    };

