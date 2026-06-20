import { StubCaptionBrokerClient, type CaptionBrokerClient } from "./captionBrokerClient";
import { deriveEntitlements, StubLicenseClient, type LicenseClient } from "./licenseClient";
import { getNativeHostBridge, type AiDirectorBridgeConfig, type NativeHostBridge } from "./nativeHostBridge";
import { isCaptionBridge, RemoteCaptionBrokerClient } from "./remoteCaptionBrokerClient";
import { isLicenseBridge, RemoteLicenseClient } from "./remoteLicenseClient";
import type { AiProductionEngine } from "./contracts";
import { HttpDirectorIntelligenceService } from "./directorIntelligenceService";
import { RuleBasedAiProductionEngine } from "./mockEngines";
import {
  ServiceBackedAiProductionEngine,
  type AutoProductionTelemetrySink
} from "./serviceBackedAiProductionEngine";

export function createLicenseClient(): LicenseClient {
  const bridge = getNativeHostBridge();
  if (isLicenseBridge(bridge)) {
    return new RemoteLicenseClient(bridge);
  }
  return new StubLicenseClient();
}

function hasAiDirectorConfig(
  bridge: unknown
): bridge is { getAiDirectorConfig(): Promise<AiDirectorBridgeConfig> } {
  return (
    typeof bridge === "object" &&
    bridge !== null &&
    typeof (bridge as { getAiDirectorConfig?: unknown }).getAiDirectorConfig === "function"
  );
}

/**
 * Default proposal-vs-taken telemetry sink: logs to the console. Hosts that have
 * a real telemetry pipeline (`services/telemetry-ingest`) can inject their own.
 */
const defaultTelemetrySink: AutoProductionTelemetrySink = (record) => {
  console.info("[auto-director]", record);
};

/**
 * Build the AI-production engine.
 *
 * - With no AI-director config on the host (the in-container/default case) the
 *   always-on heuristic `RuleBasedAiProductionEngine` is returned — zero cloud,
 *   zero behavior change.
 * - With config present, a {@link ServiceBackedAiProductionEngine} is returned.
 *   It is gated lazily on each tick by BOTH the config feature flag AND the
 *   `setAndForget` license entitlement, and falls back to the heuristic on any
 *   service timeout/error. Service URL/key come from the host bridge (config),
 *   never committed.
 */
export function createAiProductionEngine(
  license: LicenseClient = createLicenseClient(),
  telemetry: AutoProductionTelemetrySink = defaultTelemetrySink,
  bridge: NativeHostBridge | undefined = getNativeHostBridge()
): AiProductionEngine {
  if (!hasAiDirectorConfig(bridge)) {
    return new RuleBasedAiProductionEngine();
  }

  // Resolve the host config once, lazily, and cache the promise. The service
  // URL/key live in this config (host-provided env), never in the repo.
  let configPromise: Promise<AiDirectorBridgeConfig> | undefined;
  const loadConfig = () => {
    configPromise ??= bridge.getAiDirectorConfig().catch(() => ({ enabled: false }) as AiDirectorBridgeConfig);
    return configPromise;
  };

  let resolvedConfig: AiDirectorBridgeConfig | undefined;

  // serviceUrl/apiKey are resolved lazily on each propose() via these getters;
  // an empty initial config means "heuristic only" until config loads.
  const service = new HttpDirectorIntelligenceService({
    get serviceUrl() {
      return resolvedConfig?.serviceUrl;
    },
    get apiKey() {
      return resolvedConfig?.apiKey;
    },
    get timeoutMs() {
      return resolvedConfig?.timeoutMs;
    },
    get allowContentSignals() {
      return resolvedConfig?.allowContentSignals;
    }
  });

  void loadConfig().then((config) => {
    resolvedConfig = config;
  });

  return new ServiceBackedAiProductionEngine({
    service,
    telemetry,
    gate: {
      enabled: async () => {
        const config = await loadConfig();
        resolvedConfig = config;
        return Boolean(config.enabled && config.serviceUrl);
      },
      setAndForgetEntitled: async () => {
        const state = await license.getState();
        return deriveEntitlements(state, Date.now()).setAndForget;
      }
    }
  });
}

export function createCaptionBrokerClient(): CaptionBrokerClient {
  const bridge = getNativeHostBridge();
  if (isCaptionBridge(bridge)) {
    return new RemoteCaptionBrokerClient(bridge);
  }
  return new StubCaptionBrokerClient();
}