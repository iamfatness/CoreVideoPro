import type { SupportBundle } from "../src/domain/production.ts";
import { prepareSupportBundleForUpload } from "../src/engine/supportBundle.ts";

export type TelemetryConfig = {
  endpoint?: string;
  apiKey?: string;
  appVersion: string;
  platform: string;
  analyticsEnabled: boolean;
};

export type TelemetryEvent = {
  kind: "feature" | "performance";
  name: string;
  detail?: string;
  durationMs?: number;
};

export type CrashReportRequest = {
  reason: string;
  bundle?: SupportBundle;
  detail?: string;
};

export type UploadResult = {
  uploaded: boolean;
  status?: number;
  reportId?: string;
  message: string;
};

type FetchLike = typeof fetch;

export function resolveTelemetryConfig(
  env: NodeJS.ProcessEnv,
  consent: { analyticsEnabled: boolean },
  appVersion: string,
  platform: string
): TelemetryConfig {
  return {
    endpoint: env.COREVIDEO_TELEMETRY_ENDPOINT?.trim() || undefined,
    apiKey: env.COREVIDEO_TELEMETRY_API_KEY?.trim() || undefined,
    appVersion,
    platform,
    analyticsEnabled: consent.analyticsEnabled
  };
}

export function createTelemetryClient(config: TelemetryConfig, fetchImpl: FetchLike = fetch) {
  const endpoint = config.endpoint?.replace(/\/$/, "");

  async function post(path: string, body: unknown): Promise<UploadResult> {
    if (!endpoint) {
      return { uploaded: false, message: "Telemetry endpoint not configured (stub mode)." };
    }

    const headers: Record<string, string> = {
      "content-type": "application/json"
    };
    if (config.apiKey) {
      headers.authorization = `Bearer ${config.apiKey}`;
    }

    const response = await fetchImpl(`${endpoint}${path}`, {
      method: "POST",
      headers,
      body: JSON.stringify(body)
    });

    let reportId: string | undefined;
    try {
      const parsed = (await response.json()) as { reportId?: string };
      reportId = parsed.reportId;
    } catch {
      reportId = undefined;
    }

    if (!response.ok) {
      return {
        uploaded: false,
        status: response.status,
        reportId,
        message: `Telemetry upload failed (${response.status}).`
      };
    }

    return {
      uploaded: true,
      status: response.status,
      reportId,
      message: reportId ? `Uploaded report ${reportId}.` : "Upload accepted."
    };
  }

  return {
    getConfig: () => config,
    async uploadCrashReport(request: CrashReportRequest): Promise<UploadResult> {
      const bundle = request.bundle ? prepareSupportBundleForUpload(request.bundle) : undefined;
      return post("/v1/crashes", {
        at: new Date().toISOString(),
        reason: request.reason,
        detail: request.detail,
        app: {
          version: config.appVersion,
          platform: config.platform
        },
        bundle
      });
    },
    async recordEvent(event: TelemetryEvent): Promise<UploadResult> {
      if (!config.analyticsEnabled) {
        return { uploaded: false, message: "Telemetry opt-out respected." };
      }
      return post("/v1/events", {
        at: new Date().toISOString(),
        ...event,
        app: {
          version: config.appVersion,
          platform: config.platform
        }
      });
    }
  };
}

export type TelemetryClient = ReturnType<typeof createTelemetryClient>;