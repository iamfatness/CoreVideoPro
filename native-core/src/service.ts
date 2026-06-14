import { createInterface } from "node:readline";
import { stdin, stdout } from "node:process";
import { MediaCoreRuntime } from "./mediaCore.js";
import type { MediaCoreRequest, MediaCoreResponse } from "./protocol.js";

const runtime = new MediaCoreRuntime();

export function handleLine(line: string): MediaCoreResponse {
  try {
    const request = JSON.parse(line) as MediaCoreRequest;

    if (
      !request ||
      typeof request.id !== "string" ||
      (request.type !== "sync" && request.type !== "snapshot" && request.type !== "tick")
    ) {
      return invalidResponse("unknown", "Request must include id and type.");
    }

    if (request.type === "sync" && !Array.isArray(request.commands)) {
      return invalidResponse(request.id, "Sync request must include commands.");
    }

    if (request.type === "tick" && typeof request.elapsedMs !== "number") {
      return invalidResponse(request.id, "Tick request must include elapsedMs.");
    }

    return runtime.handle(request);
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
