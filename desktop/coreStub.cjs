"use strict";
var __defProp = Object.defineProperty;
var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
var __getOwnPropNames = Object.getOwnPropertyNames;
var __hasOwnProp = Object.prototype.hasOwnProperty;
var __export = (target, all) => {
  for (var name in all)
    __defProp(target, name, { get: all[name], enumerable: true });
};
var __copyProps = (to, from, except, desc) => {
  if (from && typeof from === "object" || typeof from === "function") {
    for (let key of __getOwnPropNames(from))
      if (!__hasOwnProp.call(to, key) && key !== except)
        __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
  }
  return to;
};
var __toCommonJS = (mod) => __copyProps(__defProp({}, "__esModule", { value: true }), mod);

// desktop/coreStub.ts
var coreStub_exports = {};
__export(coreStub_exports, {
  dispatchCoreRequest: () => dispatchCoreRequest,
  handleCoreRequest: () => handleCoreRequest,
  startCoreStubLoop: () => startCoreStubLoop
});
module.exports = __toCommonJS(coreStub_exports);
var import_node_readline = require("node:readline");
var import_node_process = require("node:process");
var import_node_buffer = require("node:buffer");

// desktop/captureDeviceStub.ts
var INITIAL_DEVICES = [
  {
    id: "decklink-1",
    vendor: "blackmagic",
    name: "DeckLink Mini Recorder 4K",
    inputs: [
      { id: "sdi-1", label: "SDI 1", hasEmbeddedAudio: true },
      { id: "hdmi-1", label: "HDMI", hasEmbeddedAudio: true }
    ],
    selectedInputId: "sdi-1",
    resolution: { width: 1920, height: 1080 },
    frameRate: 60,
    connectionState: "connected",
    signalPresent: true,
    droppedFrames: 0,
    audioSyncOffsetMs: 0
  },
  {
    id: "aja-io-1",
    vendor: "aja",
    name: "AJA Io 4K Plus",
    inputs: [
      { id: "sdi-1", label: "SDI 1", hasEmbeddedAudio: true },
      { id: "sdi-2", label: "SDI 2", hasEmbeddedAudio: false }
    ],
    selectedInputId: "sdi-1",
    resolution: { width: 1920, height: 1080 },
    frameRate: 30,
    connectionState: "detected",
    signalPresent: false,
    droppedFrames: 0,
    audioSyncOffsetMs: 0
  }
];
var CaptureDeviceSession = class {
  devices = INITIAL_DEVICES.map((device) => ({
    ...device,
    inputs: device.inputs.map((input) => ({ ...input }))
  }));
  list() {
    return this.devices.map((device) => ({ ...device, inputs: device.inputs.map((input) => ({ ...input })) }));
  }
  selectInput(deviceId, inputId) {
    this.devices = this.devices.map((device) => {
      if (device.id !== deviceId) {
        return device;
      }
      if (!device.inputs.some((input) => input.id === inputId)) {
        return device;
      }
      return { ...device, selectedInputId: inputId };
    });
    return this.list();
  }
  setAudioSyncOffset(deviceId, offsetMs) {
    const clamped = Math.max(-500, Math.min(500, offsetMs));
    this.devices = this.devices.map(
      (device) => device.id === deviceId ? { ...device, audioSyncOffsetMs: clamped } : device
    );
    return this.list();
  }
  connect(deviceId) {
    this.devices = this.devices.map(
      (device) => device.id === deviceId && device.connectionState !== "connected" ? { ...device, connectionState: "connected", signalPresent: true } : device
    );
    return this.list();
  }
};
var session = new CaptureDeviceSession();
function captureResponse(id, devices) {
  return { id, ok: true, type: "capture-devices", devices };
}
function handleCaptureDeviceRequest(request) {
  switch (request.type) {
    case "list-capture-devices":
      return captureResponse(request.id, session.list());
    case "select-capture-input":
      return captureResponse(request.id, session.selectInput(request.payload.deviceId, request.payload.inputId));
    case "set-capture-audio-sync-offset":
      return captureResponse(request.id, session.setAudioSyncOffset(request.payload.deviceId, request.payload.offsetMs));
    case "connect-capture-device":
      return captureResponse(request.id, session.connect(request.payload.deviceId));
  }
}

// desktop/programFramePreview.ts
var PROGRAM_FRAME_PREVIEW_MAX_WIDTH = 320;
var PROGRAM_FRAME_PREVIEW_MAX_HEIGHT = 180;
function gridColumns(layerCount) {
  if (layerCount <= 1) return 1;
  if (layerCount <= 4) return 2;
  if (layerCount <= 6) return 3;
  return 4;
}
function gridCell(layerCount, index, padding = 0.01) {
  const cols = gridColumns(layerCount);
  const rows = Math.ceil(layerCount / cols);
  const col = index % cols;
  const row = Math.floor(index / cols);
  const cellW = 1 / cols;
  const cellH = 1 / rows;
  return {
    x: col * cellW + padding,
    y: row * cellH + padding,
    width: cellW - 2 * padding,
    height: cellH - 2 * padding
  };
}
function lowerThirdOverlay() {
  return { x: 0.05, y: 0.78, width: 0.9, height: 0.16 };
}
function topRightOverlay() {
  return { x: 0.78, y: 0.04, width: 0.18, height: 0.12 };
}
function colorFromParticipantId(participantId) {
  let hash = 2166136261;
  for (let index = 0; index < participantId.length; index += 1) {
    hash ^= participantId.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  const r = 72 + (hash & 127);
  const g = 72 + (hash >> 8 & 127);
  const b = 72 + (hash >> 16 & 127);
  return 4278190080 | r << 16 | g << 8 | b;
}
function computeProgramFramePreviewSize(sourceWidth, sourceHeight) {
  if (sourceWidth <= 0 || sourceHeight <= 0) {
    return { width: 0, height: 0 };
  }
  const scale = Math.min(
    PROGRAM_FRAME_PREVIEW_MAX_WIDTH / sourceWidth,
    PROGRAM_FRAME_PREVIEW_MAX_HEIGHT / sourceHeight
  );
  return {
    width: Math.max(1, Math.min(PROGRAM_FRAME_PREVIEW_MAX_WIDTH, Math.round(sourceWidth * scale))),
    height: Math.max(1, Math.min(PROGRAM_FRAME_PREVIEW_MAX_HEIGHT, Math.round(sourceHeight * scale)))
  };
}
function setPixelBgra(pixels, width, x, y, rgba) {
  if (x < 0 || y < 0 || x >= width) {
    return;
  }
  const offset = (y * width + x) * 4;
  if (offset + 3 >= pixels.length) {
    return;
  }
  pixels[offset] = rgba & 255;
  pixels[offset + 1] = rgba >> 8 & 255;
  pixels[offset + 2] = rgba >> 16 & 255;
  pixels[offset + 3] = rgba >> 24 & 255;
}
function fillRectBgra(pixels, width, height, rect, rgba) {
  const left = Math.max(0, Math.floor(rect.x * width));
  const top = Math.max(0, Math.floor(rect.y * height));
  const right = Math.min(width, Math.ceil((rect.x + rect.width) * width));
  const bottom = Math.min(height, Math.ceil((rect.y + rect.height) * height));
  for (let py = top; py < bottom; py += 1) {
    for (let px = left; px < right; px += 1) {
      setPixelBgra(pixels, width, px, py, rgba);
    }
  }
}
function fillSyntheticProgramFramePreview(renderPlan, frames, programFrame) {
  const sourceWidth = programFrame.width > 0 ? programFrame.width : renderPlan.outputProfile.width;
  const sourceHeight = programFrame.height > 0 ? programFrame.height : renderPlan.outputProfile.height;
  const { width: previewWidth, height: previewHeight } = computeProgramFramePreviewSize(sourceWidth, sourceHeight);
  if (previewWidth <= 0 || previewHeight <= 0) {
    return void 0;
  }
  const bgra = new Uint8Array(previewWidth * previewHeight * 4);
  const background = 4278980888;
  for (let y = 0; y < previewHeight; y += 1) {
    for (let x = 0; x < previewWidth; x += 1) {
      setPixelBgra(bgra, previewWidth, x, y, background);
    }
  }
  if (renderPlan.layers.length > 0) {
    let videoIndex = 0;
    for (const layer of renderPlan.layers) {
      const renderLayer = layer;
      let rect = renderLayer.rect ?? { x: 0, y: 0, width: 0, height: 0 };
      if (rect.width <= 0 || rect.height <= 0) {
        if (layer.kind === "overlay") {
          rect = layer.layerId.includes("lower") ? lowerThirdOverlay() : topRightOverlay();
        } else {
          const videoLayerCount = renderPlan.layers.filter((entry2) => entry2.kind !== "overlay").length;
          rect = gridCell(Math.max(1, videoLayerCount), videoIndex);
          videoIndex += 1;
        }
      }
      let color = 4280956232;
      if (layer.kind !== "overlay") {
        if (layer.participantId) {
          color = colorFromParticipantId(layer.participantId);
        } else if (videoIndex > 0 && videoIndex - 1 < frames.length) {
          const frameParticipantId = frames[videoIndex - 1]?.participantId;
          if (frameParticipantId) {
            color = colorFromParticipantId(frameParticipantId);
          }
        }
      }
      fillRectBgra(bgra, previewWidth, previewHeight, rect, color);
    }
    return { width: previewWidth, height: previewHeight, bgra };
  }
  const count = frames.length;
  for (let index = 0; index < count; index += 1) {
    const layout = gridCell(Math.max(1, count), index);
    const participantId = frames[index]?.participantId;
    if (!participantId) {
      continue;
    }
    const color = colorFromParticipantId(participantId);
    fillRectBgra(bgra, previewWidth, previewHeight, layout, color);
  }
  return { width: previewWidth, height: previewHeight, bgra };
}
function encodeBgraBase64(bgra) {
  return Buffer.from(bgra).toString("base64");
}
function programFramePreviewWire(programFrame, preview, renderer = "software") {
  return {
    frameNumber: programFrame.frameNumber,
    width: preview.width,
    height: preview.height,
    renderPlanId: programFrame.renderPlanId,
    renderer,
    health: programFrame.health,
    pixelFormat: "bgra",
    bgraBase64: encodeBgraBase64(preview.bgra)
  };
}
function programFramePreviewEvent(programFrame, preview, renderer = "software") {
  if (preview.width <= 0 || preview.height <= 0 || preview.bgra.length === 0) {
    return void 0;
  }
  return {
    type: "program-frame-preview",
    preview: programFramePreviewWire(programFrame, preview, renderer)
  };
}
function programFramePreviewEventFromSnapshot(snapshot) {
  if (!snapshot.programFrame) {
    return void 0;
  }
  const preview = fillSyntheticProgramFramePreview(snapshot.renderPlan, snapshot.frames, snapshot.programFrame);
  if (!preview) {
    return void 0;
  }
  return programFramePreviewEvent(snapshot.programFrame, preview);
}

// src/engine/nativeMediaCoreAudioCaption.ts
var TARGET_LEVEL = 68;
var LIMITER_THRESHOLD = 88;
var IDLE_NATIVE_AUDIO_MIX_SESSION = {
  status: "idle",
  masterLevel: 0,
  loudnessLufs: -60,
  limiterActive: false,
  mixedFrameCount: 0,
  participants: [],
  summary: "Audio mix idle.",
  warnings: []
};
var IDLE_NATIVE_CAPTION_TRACK = {
  enabled: true,
  status: "idle",
  latencyMs: 180,
  warnings: []
};
var NativeAudioMixSessionSimulator = class {
  channels = [];
  mixedFrameCount = 0;
  sync(channels) {
    this.channels = channels.map((channel) => ({ ...channel }));
    return this.snapshot();
  }
  mix(frameCount = 1) {
    if (this.channels.length > 0) {
      this.mixedFrameCount += frameCount;
    }
    return this.snapshot();
  }
  snapshot() {
    if (this.channels.length === 0) {
      return { ...IDLE_NATIVE_AUDIO_MIX_SESSION, mixedFrameCount: this.mixedFrameCount };
    }
    const participants = this.channels.map(buildParticipantChannel);
    const audible = participants.filter((channel) => !channel.muted);
    const masterLevel = Math.min(
      100,
      Math.round(audible.reduce((total, channel) => total + channel.outputLevel, 0) / Math.max(1, audible.length) + 8)
    );
    const limiterActive = participants.some((channel) => channel.limiterActive) || masterLevel >= LIMITER_THRESHOLD;
    const boostingCount = participants.filter((channel) => channel.status === "boosting").length;
    const duckingCount = participants.filter((channel) => channel.status === "ducking").length;
    const mutedCount = participants.filter((channel) => channel.muted).length;
    const manualCount = participants.filter((channel) => channel.manualGainDb !== void 0 && channel.manualGainDb !== 0).length;
    const warnings = [];
    if (participants.some((channel) => channel.noiseSuppression && channel.inputLevel < 35)) {
      warnings.push("Noise suppression active on low-level sources.");
    }
    return {
      status: warnings.length > 0 ? "warning" : "live",
      masterLevel,
      loudnessLufs: limiterActive ? -14 : -16,
      limiterActive,
      mixedFrameCount: this.mixedFrameCount,
      participants,
      summary: buildSummary(boostingCount, duckingCount, mutedCount, manualCount),
      warnings
    };
  }
};
var NativeCaptionTrackSimulator = class {
  enabled = true;
  currentCue;
  setEnabled(enabled) {
    this.enabled = enabled;
    return this.snapshot();
  }
  pushCue(text, atMs, speaker) {
    const trimmed = text.trim();
    if (!trimmed) {
      return this.snapshot(["Caption cue ignored because text was empty."]);
    }
    this.currentCue = {
      text: trimmed,
      speaker,
      atMs,
      confidence: Math.max(82, 97 - Math.floor(trimmed.length / 28))
    };
    return this.snapshot();
  }
  snapshot(extraWarnings = []) {
    const warnings = [...extraWarnings];
    if (!this.enabled) {
      return {
        enabled: false,
        status: "idle",
        currentCue: this.currentCue,
        latencyMs: 0,
        warnings: ["Caption track disabled."]
      };
    }
    if (!this.currentCue) {
      return {
        ...IDLE_NATIVE_CAPTION_TRACK,
        enabled: true,
        warnings
      };
    }
    if (this.currentCue.text.length > 96) {
      warnings.push("Caption line is long; compact mode enabled.");
    }
    return {
      enabled: true,
      status: warnings.length > 0 ? "warning" : "live",
      currentCue: this.currentCue,
      latencyMs: 180,
      warnings
    };
  }
};
function buildParticipantChannel(channel) {
  const smartGainDb = calculateGain(channel.inputLevel);
  const gainDb = channel.muted ? -60 : clamp(smartGainDb + (channel.manualGainDb ?? 0), -12, 12);
  const noiseSuppression = channel.noiseSuppression || channel.inputLevel < 35;
  const outputLevel = channel.muted ? 0 : clamp(Math.round(channel.inputLevel + gainDb * 4), 0, 100);
  const limiterActive = outputLevel >= LIMITER_THRESHOLD;
  return {
    participantId: channel.participantId,
    inputLevel: channel.inputLevel,
    outputLevel,
    gainDb,
    manualGainDb: channel.manualGainDb,
    noiseSuppression,
    limiterActive,
    muted: channel.muted,
    status: getStatus(channel.muted, gainDb)
  };
}
function calculateGain(level) {
  const delta = TARGET_LEVEL - level;
  if (delta > 28) return 6;
  if (delta > 14) return 3;
  if (delta < -12) return -4;
  if (delta < -4) return -2;
  return 0;
}
function getStatus(muted, gainDb) {
  if (muted) return "muted";
  if (gainDb > 0) return "boosting";
  if (gainDb < 0) return "ducking";
  return "balanced";
}
function buildSummary(boostingCount, duckingCount, mutedCount, manualCount) {
  const actions = [
    boostingCount > 0 ? `${boostingCount} boosted` : "",
    duckingCount > 0 ? `${duckingCount} ducked` : "",
    mutedCount > 0 ? `${mutedCount} muted` : "",
    manualCount > 0 ? `${manualCount} manual` : ""
  ].filter(Boolean);
  return actions.length > 0 ? `${actions.join(", ")} in program mix` : "Program mix balanced";
}
function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

// src/engine/nativeMediaCoreBrandKit.ts
var IDLE_NATIVE_BRAND_KIT = {
  name: "CoreVideo Pro House",
  logoText: "CoreVideo Pro",
  brandColor: "#44c1a1",
  accentColor: "#f0a85c",
  backgroundColor: "#0c1118",
  fontFamily: "Inter",
  lowerThirdStyle: "gradient",
  appliedOverlayCount: 0,
  summary: "Brand kit idle.",
  warnings: []
};
var NativeBrandKitSimulator = class {
  brandKit = { ...IDLE_NATIVE_BRAND_KIT };
  apply(brandKit, overlayCount = 0) {
    const normalized = normalizeBrandKitSnapshot(brandKit);
    this.brandKit = {
      ...normalized,
      appliedOverlayCount: overlayCount,
      summary: overlayCount > 0 ? `${normalized.name} applied to ${overlayCount} overlays` : `${normalized.name} ready`,
      warnings: []
    };
    return this.snapshot();
  }
  snapshot() {
    return { ...this.brandKit };
  }
};
function normalizeBrandKitSnapshot(brandKit) {
  return {
    name: brandKit.name.trim() || IDLE_NATIVE_BRAND_KIT.name,
    logoText: brandKit.logoText.trim() || IDLE_NATIVE_BRAND_KIT.logoText,
    brandColor: normalizeHex(brandKit.brandColor, IDLE_NATIVE_BRAND_KIT.brandColor),
    accentColor: normalizeHex(brandKit.accentColor, IDLE_NATIVE_BRAND_KIT.accentColor),
    backgroundColor: normalizeHex(brandKit.backgroundColor, IDLE_NATIVE_BRAND_KIT.backgroundColor),
    fontFamily: brandKit.fontFamily,
    lowerThirdStyle: brandKit.lowerThirdStyle
  };
}
function normalizeHex(value, fallback) {
  return /^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/.test(value.trim()) ? value.trim().toLowerCase() : fallback;
}

// desktop/syntheticMediaCore.ts
var DEFAULT_OUTPUT_PROFILE = {
  profileId: "1080p60",
  resolution: "1920x1080",
  width: 1920,
  height: 1080,
  fps: 60,
  targetBitrateMbps: 8
};
var NEUTRAL_GRADE = {
  lut: "none",
  exposure: 0,
  contrast: 0,
  saturation: 0,
  temperature: 0
};
var SYNTHETIC_PROFILE = {
  name: "CoreVideo synthetic core (Node stub)",
  renderer: "vulkan",
  maxProgramResolution: "3840x2160",
  maxProgramFps: 60,
  maxParticipantFeeds: 8,
  maxIsoRecordings: 4,
  capabilities: [
    "zoom-raw-video",
    "zoom-raw-audio",
    "gpu-compositor",
    "scene-graph-rendering",
    "dynamic-overlays",
    "chroma-key",
    "smart-framing",
    "audio-mixer",
    "program-recording",
    "iso-recording",
    "rtmp-output",
    "ndi-output",
    "srt-output",
    "webrtc-output",
    "decklink-capture",
    "aja-capture"
  ]
};
var DEFAULT_BREAKOUT_ROOM = {
  breakoutRoomId: "main",
  breakoutRoomName: "Main room"
};
var breakoutRoomState = { ...DEFAULT_BREAKOUT_ROOM };
function createSyntheticZoomCaptureState() {
  return { joined: false, tick: 0, displayName: "Guest Producer" };
}
function synthesizeZoomJoinSnapshot(state, request) {
  state.joined = true;
  state.displayName = request.displayName.trim() || state.displayName;
  state.tick += 1;
  return synthesizeZoomSnapshot(state);
}
function synthesizeZoomLeaveSnapshot(state) {
  state.joined = false;
  state.tick += 1;
  return synthesizeZoomSnapshot(state);
}
function synthesizeZoomSnapshot(state) {
  state.tick += 1;
  if (!state.joined) {
    return { meetingState: "idle", participants: [], tick: state.tick };
  }
  return {
    meetingState: "in_meeting",
    activeSpeakerId: "operator-1",
    caption: "",
    tick: state.tick,
    participants: [
      { userId: "operator-1", displayName: state.displayName, role: "Host", videoOn: true, muted: false, talking: true, audioLevel: 76, networkQuality: "good" },
      { userId: "guest-1", displayName: "Guest 1", role: "Guest", videoOn: true, muted: false, talking: false, audioLevel: 22, networkQuality: "good" }
    ]
  };
}
function emptySourceSnapshot(sourceCount, elapsedMs) {
  return {
    adapterId: "synthetic-core",
    kind: "zoom-sdk",
    status: sourceCount > 0 ? "subscribed" : "idle",
    subscribedSourceCount: sourceCount,
    liveFrameCount: sourceCount,
    staleFrameCount: 0,
    droppedFrameCount: 0,
    lowResolutionFrameCount: 0,
    lastFrameTimestampMs: sourceCount > 0 ? elapsedMs : void 0,
    issues: [],
    warnings: []
  };
}
function zoomRosterForSync(state) {
  if (!state?.joined) {
    return { meetingState: "idle", participants: [] };
  }
  return {
    meetingState: "in_meeting",
    activeSpeakerId: "operator-1",
    participants: [
      { userId: "operator-1", displayName: state.displayName, role: "Host", videoOn: true, muted: false, talking: true, audioLevel: 76, networkQuality: "good" },
      { userId: "guest-1", displayName: "Guest 1", role: "Guest", videoOn: true, muted: false, talking: false, audioLevel: 22, networkQuality: "good" }
    ]
  };
}
function synthesizeSnapshot(commands, elapsedMs, frameNumber2, zoomState) {
  const sceneGraph = commands.find((command) => command.type === "load-scene-graph");
  const roster = commands.find((command) => command.type === "set-zoom-source-roster");
  const output = commands.find((command) => command.type === "start-program-output");
  const recordingCommand = commands.find((command) => command.type === "start-recording-session");
  const overlays = commands.filter((command) => command.type === "set-overlay-asset");
  const transforms = commands.filter((command) => command.type === "set-participant-transform");
  const audioMixCommand = commands.find((command) => command.type === "sync-participant-audio-mix");
  const captionCue = commands.find((command) => command.type === "push-caption-cue");
  const captionEnabled = commands.find((command) => command.type === "set-caption-enabled");
  const brandKitCommand = commands.find((command) => command.type === "set-brand-kit");
  const breakoutRoomChange = commands.find((command) => command.type === "simulate-breakout-room-change");
  if (breakoutRoomChange && breakoutRoomChange.type === "simulate-breakout-room-change") {
    breakoutRoomState = {
      breakoutRoomId: breakoutRoomChange.breakoutRoomId.trim() || DEFAULT_BREAKOUT_ROOM.breakoutRoomId,
      breakoutRoomName: breakoutRoomChange.breakoutRoomName.trim() || DEFAULT_BREAKOUT_ROOM.breakoutRoomName
    };
  }
  const sourceCount = roster ? roster.sources.length : 0;
  const routeCount = sceneGraph ? sceneGraph.routes.length : 0;
  const outputs = output ? [...new Set(output.destinations)] : [];
  const sceneId = sceneGraph?.sceneId;
  const renderPlan = {
    renderPlanId: `synthetic-plan-${frameNumber2}`,
    sceneId,
    outputProfile: DEFAULT_OUTPUT_PROFILE,
    colorGrade: NEUTRAL_GRADE,
    sourceCount,
    resolvedRouteCount: routeCount,
    layers: [],
    routes: [],
    warnings: []
  };
  const frames = roster ? roster.sources.map((source) => ({
    sourceId: source.sourceId,
    participantId: source.participantId,
    kind: "participant-video",
    frameNumber: frameNumber2,
    timestampMs: elapsedMs,
    width: 1280,
    height: 720,
    fps: 60,
    health: "live"
  })) : [];
  const programFrame = sceneId ? {
    frameNumber: frameNumber2,
    timestampMs: elapsedMs,
    renderPlanId: renderPlan.renderPlanId,
    sceneId,
    width: DEFAULT_OUTPUT_PROFILE.width,
    height: DEFAULT_OUTPUT_PROFILE.height,
    fps: DEFAULT_OUTPUT_PROFILE.fps,
    layerCount: sourceCount + overlays.length,
    colorGrade: NEUTRAL_GRADE,
    health: "live"
  } : void 0;
  const encoding = outputs.length > 0;
  const sourceSnapshot = emptySourceSnapshot(sourceCount, elapsedMs);
  const lastCommandTypes = commands.map((command) => command.type);
  const audioMixSession = new NativeAudioMixSessionSimulator();
  const captionTrack = new NativeCaptionTrackSimulator();
  const brandKitSimulator = new NativeBrandKitSimulator();
  if (audioMixCommand) {
    audioMixSession.sync(audioMixCommand.channels);
  }
  if (captionEnabled) {
    captionTrack.setEnabled(captionEnabled.enabled);
  }
  if (captionCue) {
    captionTrack.pushCue(captionCue.text, captionCue.atMs, captionCue.speaker);
  }
  audioMixSession.mix(frames.length > 0 ? 1 : 0);
  if (brandKitCommand) {
    brandKitSimulator.apply(
      {
        name: brandKitCommand.name,
        logoText: brandKitCommand.logoText,
        brandColor: brandKitCommand.brandColor,
        accentColor: brandKitCommand.accentColor,
        backgroundColor: brandKitCommand.backgroundColor,
        backgroundImageUrl: "",
        fontFamily: brandKitCommand.fontFamily,
        lowerThirdStyle: brandKitCommand.lowerThirdStyle
      },
      overlays.length
    );
  }
  const audioMixSnapshot = audioMixSession.snapshot();
  const captionTrackSnapshot = captionTrack.snapshot();
  const brandKitSnapshot = brandKitSimulator.snapshot();
  const programFrameCount = programFrame ? frameNumber2 : 0;
  const recording = buildRecordingSession(recordingCommand, elapsedMs, programFrameCount);
  const outputSenderSession = buildOutputSenderSession(outputs, programFrameCount);
  const encoderSession = {
    status: encoding ? "encoding" : "idle",
    renderPlanId: programFrame?.renderPlanId,
    programFrameCount: programFrame ? 1 : 0,
    targets: outputs.map((destination) => ({
      targetId: `${destination}:program`,
      destination,
      streamKind: "program",
      status: "attached",
      attachedFrameCount: programFrame ? 1 : 0
    })),
    lifecycle: {
      status: encoding ? "encoding" : "idle",
      lastTransition: encoding ? "Program output encoding." : "Encoder idle."
    },
    warnings: []
  };
  const outputHealth = buildOutputHealth(outputs, outputSenderSession, recording, encoderSession.warnings);
  const diagnostics = {
    generatedAtMs: elapsedMs,
    sceneId,
    routeCount,
    frameCount: frames.length,
    programFrameCount,
    outputs,
    outputProfile: DEFAULT_OUTPUT_PROFILE,
    outputHealth,
    outputSenderSession,
    sourceSnapshot,
    renderPlan,
    compositor: {
      status: programFrame ? "live" : "idle",
      renderPlanId: programFrame ? renderPlan.renderPlanId : void 0,
      programFrameCount: programFrame ? frameNumber2 : 0,
      droppedFrameCount: 0,
      degradedFrameCount: 0
    },
    programFrame,
    programTransport: {
      transportId: "in-process-preview",
      status: programFrame ? "publishing" : "idle",
      frameNumber: programFrame?.frameNumber,
      renderPlanId: programFrame?.renderPlanId,
      timestampMs: programFrame?.timestampMs,
      latencyMs: 0
    },
    encoderSession,
    recording,
    audioMixSession: audioMixSnapshot,
    captionTrack: captionTrackSnapshot,
    brandKit: brandKitSnapshot,
    operatorActions: [],
    eventLog: [],
    warnings: [...audioMixSnapshot.warnings, ...captionTrackSnapshot.warnings, ...brandKitSnapshot.warnings],
    lastCommandTypes
  };
  return {
    sceneId,
    routeCount,
    frameCount: frames.length,
    frames,
    sourceSnapshot,
    programFrame,
    programFrameCount: diagnostics.programFrameCount,
    programTransport: diagnostics.programTransport,
    compositor: diagnostics.compositor,
    participantTransformCount: transforms.length,
    overlayCount: overlays.length,
    outputs,
    isoParticipantIds: output ? output.isoParticipantIds : [],
    outputProfile: DEFAULT_OUTPUT_PROFILE,
    outputHealth,
    outputSenderSession,
    sourceCount,
    resolvedRouteCount: routeCount,
    renderPlan,
    encoderSession,
    recording,
    audioMixSession: audioMixSnapshot,
    captionTrack: captionTrackSnapshot,
    brandKit: brandKitSnapshot,
    operatorActions: [],
    eventLog: [],
    diagnostics,
    lastCommandTypes,
    warnings: diagnostics.warnings,
    ...(() => {
      const zoomRoster = zoomRosterForSync(zoomState);
      const inMeeting = zoomRoster.meetingState === "in_meeting" || Boolean(sceneId) || sourceCount > 0;
      return {
        meetingState: inMeeting ? "in_meeting" : "idle",
        activeSpeakerId: zoomRoster.activeSpeakerId == null ? void 0 : String(zoomRoster.activeSpeakerId),
        participants: zoomRoster.participants,
        breakoutRoomId: breakoutRoomState.breakoutRoomId,
        breakoutRoomName: breakoutRoomState.breakoutRoomName
      };
    })()
  };
}
function buildRecordingSession(recordingCommand, elapsedMs, programFrameCount) {
  if (!recordingCommand) {
    return void 0;
  }
  const targetFolder = recordingCommand.targetFolder ?? "Recordings";
  const filenamePrefix = recordingCommand.filenamePrefix ?? "program";
  return {
    sessionId: recordingCommand.sessionId ?? "recording",
    active: true,
    status: "recording",
    writerStatus: "writing",
    startedAtMs: Math.max(0, elapsedMs - 1e3),
    elapsedMs: Math.max(0, elapsedMs - 1e3),
    targetFolder,
    filenamePrefix,
    format: recordingCommand.format ?? "mp4",
    quality: recordingCommand.quality ?? "high",
    encoder: {
      codec: "h264",
      hardwareAccelerated: true,
      targetBitrateMbps: 18
    },
    estimatedDiskRateMBps: 4.99,
    programPath: `${targetFolder}/${filenamePrefix}-program-0.mp4`,
    streams: [],
    totalFramesWritten: programFrameCount,
    totalDroppedFrames: 0,
    totalBytesWritten: programFrameCount > 0 ? 4096 : 0
  };
}
function buildOutputSenderSession(outputs, programFrameCount) {
  const streamDestinations = outputs.filter(
    (destination) => destination !== "recording"
  );
  const senders = streamDestinations.map((destination) => ({
    senderId: `${destination}:program`,
    destination,
    status: "live",
    framesSent: programFrameCount,
    retryCount: 0,
    latencyMs: 2100,
    bitrateMbps: 6
  }));
  return {
    status: senders.length > 0 ? "live" : "idle",
    activeSenderCount: senders.length,
    senders,
    warnings: []
  };
}
function buildOutputHealth(outputs, senderSession, recording, encoderWarnings) {
  const health = [];
  for (const destination of outputs) {
    if (destination === "recording") {
      const status = recording?.status === "failed" ? "failed" : recording?.status === "warning" ? "warning" : recording?.active ? "live" : "idle";
      health.push({
        destination: "recording",
        status,
        message: recording?.programPath ?? "Recording idle.",
        droppedFrames: recording?.totalDroppedFrames ?? 0
      });
      continue;
    }
    const sender = senderSession.senders.find((item) => item.destination === destination);
    health.push({
      destination,
      status: sender?.status === "failed" ? "failed" : sender?.status === "warning" ? "warning" : sender?.status === "live" || sender?.status === "starting" ? "live" : "idle",
      message: sender?.warning ?? `${destination.toUpperCase()} sender ${sender?.status ?? "idle"}.`,
      droppedFrames: 0
    });
  }
  for (const warning of encoderWarnings) {
    health.push({
      destination: "recording",
      status: "warning",
      message: warning,
      droppedFrames: 0
    });
  }
  return health;
}
function synthesizeSpineSnapshot(payload, elapsedMs) {
  const participantsById = new Map(payload.participants.map((p) => [p.sdkUserId, p]));
  const subscriptions = payload.subscriptions.map((sub) => {
    const participant = participantsById.get(sub.participantId);
    const videoOff = participant && (sub.kind === "participant-video" || sub.kind === "screen-share") && !participant.videoOn;
    const status = videoOff ? "failed" : "subscribed";
    const isAudio = sub.kind === "participant-audio";
    return {
      ...sub,
      subscriptionId: `${sub.kind}:${sub.participantId}:${sub.purpose}`,
      displayName: participant?.displayName,
      status,
      lastResultCode: videoOff ? "video-off" : "ok",
      deliveredWidth: sub.kind === "screen-share" ? 1920 : 1280,
      deliveredHeight: sub.kind === "screen-share" ? 1080 : 720,
      deliveredFps: 30,
      framesReceived: isAudio || videoOff ? 0 : Math.max(1, Math.floor(elapsedMs / 33)),
      audioPacketsReceived: isAudio && !videoOff ? Math.max(1, Math.floor(elapsedMs / 20)) : 0
    };
  });
  const activeSpeaker = payload.participants.find((p) => p.talking);
  const screenShare = payload.participants.find((p) => p.sharingScreen);
  const subscribedFeeds = subscriptions.filter(
    (s) => s.status !== "failed" && s.kind === "participant-video"
  ).length;
  const totalAudioPackets = subscriptions.reduce((t, s) => t + s.audioPacketsReceived, 0);
  return {
    meetingState: payload.blocked ? "error" : "in-meeting",
    sdkVersion: payload.readiness.sdkVersion,
    participantCount: payload.participants.length,
    activeSpeakerId: activeSpeaker?.sdkUserId,
    screenShareParticipantId: screenShare?.sdkUserId,
    participants: payload.participants,
    subscriptions,
    recording: {
      evidence: {
        programFramesWritten: subscribedFeeds > 0 ? Math.max(1, Math.floor(elapsedMs / 33)) : 0,
        isoFramesWritten: 0,
        audioPacketsObserved: totalAudioPackets,
        subscribedVideoFeeds: subscribedFeeds
      }
    },
    warnings: [...payload.warnings],
    events: ["zoom-media-spine-sync accepted by Node stub core.", payload.summary]
  };
}

// desktop/coreStub.ts
var frameNumber = 0;
var zoomCaptureState = createSyntheticZoomCaptureState();
var ZOOM_THUMB_WIDTH = 160;
var ZOOM_THUMB_HEIGHT = 90;
var ZOOM_STUB_PARTICIPANTS = ["p1", "p2", "p3"];
var ZOOM_PARTICIPANT_COLORS = {
  p1: [161, 193, 68],
  p2: [92, 168, 240],
  p3: [200, 140, 100]
};
function synthesizeZoomVideoFrameEvent(participantId, frameId) {
  const width = ZOOM_THUMB_WIDTH;
  const height = ZOOM_THUMB_HEIGHT;
  const bgra = import_node_buffer.Buffer.alloc(width * height * 4);
  const [b, g, r] = ZOOM_PARTICIPANT_COLORS[participantId] ?? [80, 60, 40];
  const pulse = 0.72 + 0.28 * Math.sin(frameId / 8);
  for (let index = 0; index < width * height; index += 1) {
    const offset = index * 4;
    bgra[offset] = Math.round(b * pulse);
    bgra[offset + 1] = Math.round(g * pulse);
    bgra[offset + 2] = Math.round(r * pulse);
    bgra[offset + 3] = 255;
  }
  return {
    type: "zoom-video-frame",
    frame: {
      participantId,
      width,
      height,
      frameId,
      bgraBase64: import_node_buffer.Buffer.from(bgra).toString("base64")
    }
  };
}
function handleCoreRequest(raw) {
  let request;
  try {
    request = JSON.parse(raw);
  } catch {
    return { id: "unknown", ok: false, error: { code: "invalid-request", message: "Unparseable request." } };
  }
  if (!request || typeof request.id !== "string" || typeof request.type !== "string") {
    return { id: "unknown", ok: false, error: { code: "invalid-request", message: "Request needs id and type." } };
  }
  switch (request.type) {
    case "handshake":
      return { id: request.id, ok: true, type: "handshake", profile: SYNTHETIC_PROFILE };
    case "ping":
      return { id: request.id, ok: true, type: "ping" };
    case "media-core-sync": {
      const sync = request;
      if (!Array.isArray(sync.commands) || typeof sync.elapsedMs !== "number") {
        return { id: request.id, ok: false, error: { code: "invalid-request", message: "sync needs commands and elapsedMs." } };
      }
      frameNumber += 1;
      const snapshot = synthesizeSnapshot(sync.commands, sync.elapsedMs, frameNumber, zoomCaptureState);
      return {
        id: request.id,
        ok: true,
        type: "media-core-sync",
        snapshot
      };
    }
    case "zoom-join": {
      const join = request;
      if (!join.payload || typeof join.payload.displayName !== "string") {
        return { id: request.id, ok: false, error: { code: "invalid-request", message: "zoom-join needs a payload." } };
      }
      return { id: request.id, ok: true, type: "zoom-join", snapshot: synthesizeZoomJoinSnapshot(zoomCaptureState, join.payload) };
    }
    case "zoom-leave":
      return { id: request.id, ok: true, type: "zoom-leave", snapshot: synthesizeZoomLeaveSnapshot(zoomCaptureState) };
    case "zoom-snapshot":
      return { id: request.id, ok: true, type: "zoom-snapshot", snapshot: synthesizeZoomSnapshot(zoomCaptureState) };
    case "zoom-media-spine-sync": {
      const spine = request;
      if (!spine.spinePayload || typeof spine.elapsedMs !== "number") {
        return { id: request.id, ok: false, error: { code: "invalid-request", message: "zoom-media-spine-sync needs spinePayload and elapsedMs." } };
      }
      return {
        id: request.id,
        ok: true,
        type: "zoom-media-spine-sync",
        spineSnapshot: synthesizeSpineSnapshot(spine.spinePayload, spine.elapsedMs)
      };
    }
    case "list-capture-devices":
    case "select-capture-input":
    case "set-capture-audio-sync-offset":
    case "connect-capture-device":
      return handleCaptureDeviceRequest(request);
    case "__crash":
      import_node_process.stdout.write(`${JSON.stringify({ id: request.id, ok: false, error: { code: "media-core-failed", message: "crashing" } })}
`);
      process.exit(1);
      return null;
    default:
      return { id: request.id, ok: false, error: { code: "invalid-request", message: `Unsupported type ${request.type}.` } };
  }
}
function dispatchCoreRequest(raw) {
  const response = handleCoreRequest(raw);
  if (!response) {
    return null;
  }
  if (response.ok && response.type === "media-core-sync" && "snapshot" in response) {
    const events = ZOOM_STUB_PARTICIPANTS.map(
      (participantId) => synthesizeZoomVideoFrameEvent(participantId, response.snapshot.programFrame?.frameNumber ?? frameNumber)
    );
    const previewEvent = programFramePreviewEventFromSnapshot(response.snapshot);
    if (previewEvent) {
      events.push(previewEvent);
    }
    return events.length > 0 ? { response, events } : { response };
  }
  return { response };
}
function startCoreStubLoop() {
  const lines = (0, import_node_readline.createInterface)({ input: import_node_process.stdin });
  lines.on("line", (line) => {
    if (line.trim().length === 0) {
      return;
    }
    const dispatch = dispatchCoreRequest(line);
    if (!dispatch) {
      return;
    }
    import_node_process.stdout.write(`${JSON.stringify(dispatch.response)}
`);
    for (const event of dispatch.events ?? []) {
      import_node_process.stdout.write(`${JSON.stringify(event)}
`);
    }
  });
}
var entry = process.argv[1] ?? "";
if (entry.endsWith("coreStub.ts") || entry.endsWith("coreStub.js") || entry.endsWith("coreStub.cjs")) {
  startCoreStubLoop();
}
// Annotate the CommonJS export names for ESM import in node:
0 && (module.exports = {
  dispatchCoreRequest,
  handleCoreRequest,
  startCoreStubLoop
});
