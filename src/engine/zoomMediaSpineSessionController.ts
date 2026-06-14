import type { ProductionState } from "../domain/production";
import { executeZoomMediaSpineServicePlan, type ZoomMediaSpineServiceExecutionResult, type ZoomMediaSpineServiceTransport } from "./zoomMediaSpineServiceExecutor";
import {
  buildZoomMediaSpineServicePlan,
  type ZoomMediaSpineServiceJoin,
  type ZoomMediaSpineServicePlanOptions
} from "./zoomMediaSpineServicePlan";
import { buildZoomMediaSpineSyncPayload, type ZoomMediaSpineSyncOptions } from "./zoomMediaSpineSync";
import type { ZoomSdkReadinessInput } from "./zoomSdkReadiness";

export type ZoomMediaSpineSessionControllerOptions = {
  syncOptions?: ZoomMediaSpineSyncOptions;
  requestIdPrefix?: string;
  includeSnapshot?: boolean;
};

export type ZoomMediaSpineSessionRunOptions = ZoomMediaSpineSessionControllerOptions & {
  elapsedMs: number;
};

export type ZoomMediaSpineSessionJoinOptions = ZoomMediaSpineSessionRunOptions & {
  join: ZoomMediaSpineServiceJoin;
};

export class ZoomMediaSpineSessionController {
  private recordingActive = false;

  constructor(private readonly transport: ZoomMediaSpineServiceTransport) {}

  async joinProduction(
    state: ProductionState,
    readinessInput: ZoomSdkReadinessInput,
    options: ZoomMediaSpineSessionJoinOptions
  ): Promise<ZoomMediaSpineServiceExecutionResult> {
    const result = await this.run(state, readinessInput, {
      ...options,
      mode: "join",
      join: options.join
    });
    this.updateRecordingState(state, result);
    return result;
  }

  async syncProduction(
    state: ProductionState,
    readinessInput: ZoomSdkReadinessInput,
    options: ZoomMediaSpineSessionRunOptions
  ): Promise<ZoomMediaSpineServiceExecutionResult> {
    const result = await this.run(state, readinessInput, {
      ...options,
      mode: "sync",
      previousRecordingActive: this.recordingActive
    });
    this.updateRecordingState(state, result);
    return result;
  }

  async leave(options: Pick<ZoomMediaSpineSessionRunOptions, "elapsedMs" | "requestIdPrefix" | "includeSnapshot">): Promise<ZoomMediaSpineServiceExecutionResult> {
    const plan = buildZoomMediaSpineServicePlan(
      {
        readiness: {
          status: "ready",
          platform: "windows",
          sdkVersion: "unknown",
          checks: [],
          blockers: [],
          warnings: [],
          summary: "Leave does not require SDK readiness."
        },
        participants: [],
        subscriptions: [],
        blocked: false,
        warnings: [],
        summary: "Leave requested."
      },
      {
        platform: "windows",
        sdkRuntimePresent: true,
        appKeyPresent: true,
        oauthConfigured: true,
        jwtBrokerConfigured: true,
        rawVideoEnabled: true,
        rawAudioEnabled: true,
        rawShareEnabled: true
      },
      {
        mode: "leave",
        elapsedMs: options.elapsedMs,
        requestIdPrefix: options.requestIdPrefix,
        includeSnapshot: options.includeSnapshot
      }
    );
    const result = await executeZoomMediaSpineServicePlan(plan, this.transport);
    if (result.ok) {
      this.recordingActive = false;
    }
    return result;
  }

  isRecordingActive() {
    return this.recordingActive;
  }

  private async run(
    state: ProductionState,
    readinessInput: ZoomSdkReadinessInput,
    options: ZoomMediaSpineSessionRunOptions & Pick<ZoomMediaSpineServicePlanOptions, "mode" | "join" | "previousRecordingActive">
  ) {
    const payload = buildZoomMediaSpineSyncPayload(state, readinessInput, options.syncOptions);
    const plan = buildZoomMediaSpineServicePlan(payload, readinessInput, {
      mode: options.mode,
      join: options.join,
      elapsedMs: options.elapsedMs,
      requestIdPrefix: options.requestIdPrefix,
      includeSnapshot: options.includeSnapshot,
      previousRecordingActive: options.previousRecordingActive
    });

    return executeZoomMediaSpineServicePlan(plan, this.transport);
  }

  private updateRecordingState(state: ProductionState, result: ZoomMediaSpineServiceExecutionResult) {
    if (!result.ok) {
      return;
    }

    this.recordingActive = state.recording;
  }
}
