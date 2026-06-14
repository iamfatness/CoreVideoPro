import type {
  AiProductionEngine,
  AudioMixEngine,
  CaptionOverlayEngine,
  CaptureDeviceEngine,
  DiagnosticsEngine,
  OutputEngine,
  PresetEngine,
  ZoomCaptureEngine
} from "./contracts";
import { MockCaptureDeviceEngine } from "./captureDevices";
import { MockOutputEngine, MockZoomCaptureEngine, RuleBasedAiProductionEngine } from "./mockEngines";
import { NativeCaptureDeviceEngineAdapter } from "./nativeCaptureDeviceEngineAdapter";
import { NativeOutputEngineAdapter } from "./nativeOutputEngineAdapter";
import { NativeZoomEngineAdapter } from "./nativeZoomEngineAdapter";
import type { NativeZoomTransport } from "./nativeBridgeProtocol";
import { SimulatedAudioMixEngine } from "./audioMix";
import { SimulatedCaptionOverlayEngine } from "./captionOverlay";
import { InMemoryPresetEngine } from "./presets";
import { InMemoryDiagnosticsEngine } from "./supportBundle";

export type EngineBundle = {
  zoom: ZoomCaptureEngine;
  ai: AiProductionEngine;
  output: OutputEngine;
  audio: AudioMixEngine;
  captions: CaptionOverlayEngine;
  presets: PresetEngine;
  diagnostics: DiagnosticsEngine;
  captureDevices: CaptureDeviceEngine;
};

export function createMockEngineBundle(): EngineBundle {
  return {
    zoom: new MockZoomCaptureEngine(),
    ai: new RuleBasedAiProductionEngine(),
    output: new MockOutputEngine(),
    audio: new SimulatedAudioMixEngine(),
    captions: new SimulatedCaptionOverlayEngine(),
    presets: new InMemoryPresetEngine(),
    diagnostics: new InMemoryDiagnosticsEngine(),
    captureDevices: new MockCaptureDeviceEngine()
  };
}

export function createNativeZoomEngineBundle(transport: NativeZoomTransport): EngineBundle {
  return {
    zoom: new NativeZoomEngineAdapter(transport),
    ai: new RuleBasedAiProductionEngine(),
    output: new NativeOutputEngineAdapter(transport),
    audio: new SimulatedAudioMixEngine(),
    captions: new SimulatedCaptionOverlayEngine(),
    presets: new InMemoryPresetEngine(),
    diagnostics: new InMemoryDiagnosticsEngine(),
    captureDevices: new NativeCaptureDeviceEngineAdapter(transport)
  };
}
