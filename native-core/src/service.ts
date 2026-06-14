import { createInterface } from "node:readline";
import { stdin, stdout } from "node:process";
import { MediaCoreRuntime } from "./mediaCore.js";
import type { MediaCoreRequest, MediaCoreResponse } from "./protocol.js";
import { ZoomMediaSpineRuntime } from "./zoomMediaSpine.js";
import type { ZoomMediaSpineServiceRequest, ZoomMediaSpineServiceResponse } from "./zoomMediaSpineServiceProtocol.js";

const runtime = new MediaCoreRuntime();
let zoomRuntime: ZoomMediaSpineRuntime | undefined;

type RawServiceRequest = {
  id: string;
  type: string;
} & Record<string, unknown>;

export function handleLine(line: string): MediaCoreResponse | ZoomMediaSpineServiceResponse {
  try {
    const request = JSON.parse(line) as Partial<RawServiceRequest>;

    if (!request || typeof request.id !== "string" || typeof request.type !== "string") {
      return invalidResponse("unknown", "Request must include id and type.");
    }

    const validatedRequest = request as RawServiceRequest;

    if (isZoomMediaSpineRequest(validatedRequest)) {
      return handleZoomMediaSpineRequest(validatedRequest);
    }

    if (!isMediaCoreRequest(validatedRequest)) {
      return invalidResponse(validatedRequest.id, "Request type is not supported.");
    }

    if (validatedRequest.type === "sync" && !Array.isArray(validatedRequest.commands)) {
      return invalidResponse(validatedRequest.id, "Sync request must include commands.");
    }

    if (validatedRequest.type === "tick" && typeof validatedRequest.elapsedMs !== "number") {
      return invalidResponse(validatedRequest.id, "Tick request must include elapsedMs.");
    }

    return runtime.handle(validatedRequest);
  } catch (error) {
    return invalidResponse("unknown", error instanceof Error ? error.message : "Unable to parse request.");
  }
}

if (process.argv[1]?.endsWith("service.ts") || process.argv[1]?.endsWith("service.js")) {
  const lines = createInterface({ input: stdin });
  lines.on("line", (line: string) => {
    stdout.write(`${JSON.stringify(handleLine(line))}\n`);
  });
}

function handleZoomMediaSpineRequest(request: ZoomMediaSpineServiceRequest): ZoomMediaSpineServiceResponse {
  if (request.type === "zoom-configure") {
    zoomRuntime = new ZoomMediaSpineRuntime(request.config);
    return {
      id: request.id,
      ok: true,
      zoom: zoomRuntime.snapshot()
    };
  }

  if (!zoomRuntime) {
    return zoomInvalidResponse(request.id, "zoom-runtime-unconfigured", "Zoom media spine runtime is not configured.");
  }

  if (request.type === "zoom-join") {
    return zoomOk(request.id, zoomRuntime.join(request.join, request.participants ?? []));
  }

  if (request.type === "zoom-roster") {
    return zoomOk(request.id, zoomRuntime.replaceRoster(request.participants));
  }

  if (request.type === "zoom-subscriptions") {
    return zoomOk(request.id, zoomRuntime.syncSubscriptions(request.subscriptions));
  }

  if (request.type === "zoom-start-recording") {
    return zoomOk(request.id, zoomRuntime.startRecording(request.targets));
  }

  if (request.type === "zoom-stop-recording") {
    return zoomOk(request.id, zoomRuntime.stopRecording());
  }

  if (request.type === "zoom-tick") {
    if (typeof request.elapsedMs !== "number") {
      return zoomInvalidResponse(request.id, "invalid-request", "Zoom tick request must include elapsedMs.");
    }

    return zoomOk(request.id, zoomRuntime.tick(request.elapsedMs));
  }

  if (request.type === "zoom-leave") {
    return zoomOk(request.id, zoomRuntime.leave());
  }

  return zoomOk(request.id, zoomRuntime.snapshot());
}

function isZoomMediaSpineRequest(request: RawServiceRequest): request is ZoomMediaSpineServiceRequest {
  return request.type.startsWith("zoom-");
}

function isMediaCoreRequest(request: RawServiceRequest): request is MediaCoreRequest {
  return request.type === "sync" || request.type === "snapshot" || request.type === "tick";
}

function zoomOk(id: string, zoom: ReturnType<ZoomMediaSpineRuntime["snapshot"]>): ZoomMediaSpineServiceResponse {
  return {
    id,
    ok: true,
    zoom
  };
}

function zoomInvalidResponse(
  id: string,
  code: "invalid-request" | "zoom-runtime-unconfigured",
  message: string
): ZoomMediaSpineServiceResponse {
  return {
    id,
    ok: false,
    error: {
      code,
      message
    }
  };
}

function invalidResponse(id: string, message: string): MediaCoreResponse {
  return {
    id,
    ok: false,
    error: {
      code: "invalid-request",
      message
    }
  };
}
