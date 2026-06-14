import type { ZoomMediaSpineNativeSnapshot } from "./zoomMediaSpineNativeSync";
import type { ZoomMediaSpineServicePlan, ZoomMediaSpineServiceRequest } from "./zoomMediaSpineServicePlan";

export type ZoomMediaSpineServiceResponse =
  | {
      id: string;
      ok: true;
      zoom: ZoomMediaSpineNativeSnapshot;
    }
  | {
      id: string;
      ok: false;
      error: {
        code: "invalid-request" | "zoom-runtime-unconfigured" | "transport-error" | string;
        message: string;
      };
    };

export interface ZoomMediaSpineServiceTransport {
  request(request: ZoomMediaSpineServiceRequest): Promise<ZoomMediaSpineServiceResponse>;
}

export type ZoomMediaSpineServiceExecutionStep = {
  requestId: string;
  requestType: ZoomMediaSpineServiceRequest["type"];
  ok: boolean;
  errorCode?: string;
  message?: string;
};

export type ZoomMediaSpineServiceExecutionResult = {
  ok: boolean;
  blocked: boolean;
  finalSnapshot?: ZoomMediaSpineNativeSnapshot;
  steps: ZoomMediaSpineServiceExecutionStep[];
  warnings: string[];
  summary: string;
};

export async function executeZoomMediaSpineServicePlan(
  plan: ZoomMediaSpineServicePlan,
  transport: ZoomMediaSpineServiceTransport
): Promise<ZoomMediaSpineServiceExecutionResult> {
  const steps: ZoomMediaSpineServiceExecutionStep[] = [];
  let finalSnapshot: ZoomMediaSpineNativeSnapshot | undefined;

  for (const request of plan.requests) {
    try {
      const response = await transport.request(request);
      if (!response.ok) {
        steps.push({
          requestId: request.id,
          requestType: request.type,
          ok: false,
          errorCode: response.error.code,
          message: response.error.message
        });

        return executionResult(false, plan, steps, finalSnapshot, `${request.type} failed: ${response.error.message}`);
      }

      finalSnapshot = response.zoom;
      steps.push({
        requestId: request.id,
        requestType: request.type,
        ok: true
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Zoom media spine transport failed.";
      steps.push({
        requestId: request.id,
        requestType: request.type,
        ok: false,
        errorCode: "transport-error",
        message
      });

      return executionResult(false, plan, steps, finalSnapshot, `${request.type} transport error: ${message}`);
    }
  }

  if (!finalSnapshot) {
    return executionResult(false, plan, steps, undefined, "Zoom media spine service plan completed without a snapshot.");
  }

  return executionResult(!plan.blocked, plan, steps, finalSnapshot, plan.blocked ? plan.summary : `${steps.length} Zoom media spine request${steps.length === 1 ? "" : "s"} executed.`);
}

function executionResult(
  ok: boolean,
  plan: ZoomMediaSpineServicePlan,
  steps: ZoomMediaSpineServiceExecutionStep[],
  finalSnapshot: ZoomMediaSpineNativeSnapshot | undefined,
  summary: string
): ZoomMediaSpineServiceExecutionResult {
  const failedStep = steps.find((step) => !step.ok);
  return {
    ok,
    blocked: plan.blocked || Boolean(failedStep),
    finalSnapshot,
    steps,
    warnings: [
      ...plan.warnings,
      ...(failedStep?.message ? [failedStep.message] : []),
      ...(finalSnapshot?.warnings ?? [])
    ],
    summary
  };
}
