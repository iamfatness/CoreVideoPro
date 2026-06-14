import React from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import { createMockEngineBundle, createNativeZoomEngineBundle } from "./engine/engineBundle";
import { createNativeHostTransport, getNativeHostBridge } from "./engine/nativeHostBridge";
import "./styles.css";

const nativeBridge = getNativeHostBridge();
const engines = nativeBridge
  ? createNativeZoomEngineBundle(createNativeHostTransport(nativeBridge))
  : createMockEngineBundle();

createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <App engines={engines} />
  </React.StrictMode>
);
