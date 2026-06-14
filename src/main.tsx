import React from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import { createMockEngineBundle, createNativeZoomEngineBundle } from "./engine/engineBundle";
import { createNativeHostTransport, getNativeHostBridge } from "./engine/nativeHostBridge";
import { describeRuntimeEnvironment } from "./engine/runtimeEnvironment";
import { connectFrameStoreToBridge, zoomVideoFrameStore } from "./engine/zoomVideoFrames";
import "./styles.css";

const nativeBridge = getNativeHostBridge();
const runtime = describeRuntimeEnvironment(nativeBridge, nativeBridge?.mediaCoreProfile);

// Stream live per-participant video frames from the native Zoom engine into the
// renderer's frame store. No-op on hosts without the engine (in-container mock).
connectFrameStoreToBridge(zoomVideoFrameStore, nativeBridge);

const engines = nativeBridge
  ? createNativeZoomEngineBundle(createNativeHostTransport(nativeBridge), nativeBridge)
  : createMockEngineBundle();

createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <App engines={engines} runtime={runtime} />
  </React.StrictMode>
);
