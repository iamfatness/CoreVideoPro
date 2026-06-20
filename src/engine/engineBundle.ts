import type {
  AiProductionEngine,
  AiStudioEngine,
  AudioMixEngine,
  CaptionOverlayEngine,
  CaptureDeviceEngine,
  DiagnosticsEngine,
  MediaCoreSyncEngine,
  OutputEngine,
  PresetEngine,
  ZoomCaptureEngine
} from "./contracts";
import { MockCaptureDeviceEngine } from "./captureDevices";
import { MockOutputEngine, MockZoomCaptureEngine, RuleBasedAiProductionEngine } from "./mockEngines";
import { localDirectorProvider } from "./localDirectorProvider";
import { NativeCaptureDeviceEngineAdapter } from "./nativeCaptureDeviceEngineAdapter";
import { NativeOutputEngineAdapter } from "./nativeOutputEngineAdapter";
import { NativeZoomEngineAdapter } from "./nativeZoomEngineAdapter";
import type { NativeZoomTransport } from "./nativeBridgeProtocol";
import { SimulatedAudioMixEngine } from "./audioMix";
import { SimulatedCaptionOverlayEngine } from "./captionOverlay";
import { RuleBasedAiStudioEngine } from "./aiStudio";
import { InMemoryMediaCoreSyncEngine, NativeHostMediaCoreSyncEngine } from "./mediaCoreSync";
import { InMemoryPresetEngine } from "./presets";
import { InMemoryDiagnosticsEngine } from "./supportBundle";
import type { CaptionBrokerClient } from "./captionBrokerClient";
import { createCaptionBrokerClient, createLicenseClient } from "./commerceClients";
import type { LicenseClient } from "./licenseClient";
import type { NativeHostBridge } from "./nativeHostBridge";
import { ZoomMediaSpineSessionController } from "./zoomMediaSpineSessionController";
import {
  InMemoryZoomMediaSpineTransport,
  NativeHostZoomMediaSpineTransport,
  createZoomMediaSpineTransport
} from "./zoomMediaSpineTransport";

export type EngineBundle = {
  zoom: ZoomCaptureEngine;
  ai: AiProductionEngine;
  aiStudio: AiStudioEngine;
  output: OutputEngine;
  audio: AudioMixEngine;
  captions: CaptionOverlayEngine;
  presets: PresetEngine;
  diagnostics: DiagnosticsEngine;
  captureDevices: CaptureDeviceEngine;
  mediaCore: MediaCoreSyncEngine;
  spineController: ZoomMediaSpineSessionController;
  spineTransport: InMemoryZoomMediaSpineTransport | NativeHostZoomMediaSpineTransport;
  license: LicenseClient;
  captionBroker: CaptionBrokerClient;
};

export function createMockEngineBundle(): EngineBundle {
  const spineTransport = new InMemoryZoomMediaSpineTransport();
  return {
    zoom: new MockZoomCaptureEngine(),
    ai: new RuleBasedAiProductionEngine({ directorProvider: localDirectorProvider }),
    aiStudio: new RuleBasedAiStudioEngine(),
    output: new MockOutputEngine(),
    audio: new SimulatedAudioMixEngine(),
    captions: new SimulatedCaptionOverlayEngine(),
    presets: new InMemoryPresetEngine(),
    diagnostics: new InMemoryDiagnosticsEngine(),
    captureDevices: new MockCaptureDeviceEngine(),
    mediaCore: new InMemoryMediaCoreSyncEngine(),
    spineController: new ZoomMediaSpineSessionController(spineTransport),
    spineTransport,
    license: createLicenseClient(),
    captionBroker: createCaptionBrokerClient()
  };
}

export function createNativeZoomEngineBundle(transport: NativeZoomTransport, bridge?: NativeHostBridge): EngineBundle {
  const { transport: spineTransport, nativeTransport } = createZoomMediaSpineTransport(bridge);
  return {
    zoom: new NativeZoomEngineAdapter(transport),
    ai: new RuleBasedAiProductionEngine({ directorProvider: localDirectorProvider }),
    aiStudio: new RuleBasedAiStudioEngine(),
    output: new NativeOutputEngineAdapter(transport),
    audio: new SimulatedAudioMixEngine(),
    captions: new SimulatedCaptionOverlayEngine(),
    presets: new InMemoryPresetEngine(),
    diagnostics: new InMemoryDiagnosticsEngine(),
    captureDevices: new NativeCaptureDeviceEngineAdapter(transport),
    mediaCore: bridge ? new NativeHostMediaCoreSyncEngine(bridge) : new InMemoryMediaCoreSyncEngine(),
    spineController: new ZoomMediaSpineSessionController(spineTransport),
    spineTransport: nativeTransport ?? spineTransport,
    license: createLicenseClient(),
    captionBroker: createCaptionBrokerClient()
  };
}
