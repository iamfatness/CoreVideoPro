import React from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import { createMockEngineBundle, createNativeZoomEngineBundle } from "./engine/engineBundle";
import { createNativeHostTransport, getNativeHostBridge } from "./engine/nativeHostBridge";
import { describeRuntimeEnvironment } from "./engine/runtimeEnvironment";
import "./styles.css";

const nativeBridge = getNativeHostBridge();
const runtime = describeRuntimeEnvironment(nativeBridge, nativeBridge?.mediaCoreProfile);
const engines = nativeBridge
  ? createNativeZoomEngineBundle(createNativeHostTransport(nativeBridge))
  : createMockEngineBundle();

createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <App engines={engines} runtime={runtime} />
  </React.StrictMode>
);
