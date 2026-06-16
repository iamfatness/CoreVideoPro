/**
 * Program-frame preview synthesis for the Node media-core stub.
 * Mirrors native/src/modules/ProgramFramePreview.cpp so unsolicited
 * `program-frame-preview` events match the C++ stub build.
 */
import type { ProgramFramePreviewWire } from "./coreProtocol.ts";
import type {
  NativeMediaCoreFrame,
  NativeMediaCoreProgramFrame,
  NativeMediaCoreRenderPlan
} from "../src/engine/nativeMediaCoreProtocol";

export const PROGRAM_FRAME_PREVIEW_MAX_WIDTH = 320;
export const PROGRAM_FRAME_PREVIEW_MAX_HEIGHT = 180;

export type ProgramFramePreviewPixels = {
  width: number;
  height: number;
  bgra: Uint8Array;
};

type LayerRect = {
  x: number;
  y: number;
  width: number;
  height: number;
};

type RenderPlanLayer = NativeMediaCoreRenderPlan["layers"][number] & {
  rect?: LayerRect;
};

function gridColumns(layerCount: number): number {
  if (layerCount <= 1) return 1;
  if (layerCount <= 4) return 2;
  if (layerCount <= 6) return 3;
  return 4;
}

function gridCell(layerCount: number, index: number, padding = 0.01): LayerRect {
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

function lowerThirdOverlay(): LayerRect {
  return { x: 0.05, y: 0.78, width: 0.9, height: 0.16 };
}

function topRightOverlay(): LayerRect {
  return { x: 0.78, y: 0.04, width: 0.18, height: 0.12 };
}

/** FNV-1a tint used by the native compositor for participant tiles. */
export function colorFromParticipantId(participantId: string): number {
  let hash = 2166136261;
  for (let index = 0; index < participantId.length; index += 1) {
    hash ^= participantId.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  const r = 72 + (hash & 0x7f);
  const g = 72 + ((hash >> 8) & 0x7f);
  const b = 72 + ((hash >> 16) & 0x7f);
  return 0xff000000 | (r << 16) | (g << 8) | b;
}

export function computeProgramFramePreviewSize(sourceWidth: number, sourceHeight: number): { width: number; height: number } {
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

function setPixelBgra(pixels: Uint8Array, width: number, x: number, y: number, rgba: number): void {
  if (x < 0 || y < 0 || x >= width) {
    return;
  }
  const offset = (y * width + x) * 4;
  if (offset + 3 >= pixels.length) {
    return;
  }
  pixels[offset] = rgba & 0xff;
  pixels[offset + 1] = (rgba >> 8) & 0xff;
  pixels[offset + 2] = (rgba >> 16) & 0xff;
  pixels[offset + 3] = (rgba >> 24) & 0xff;
}

function fillRectBgra(
  pixels: Uint8Array,
  width: number,
  height: number,
  rect: LayerRect,
  rgba: number
): void {
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

export function fillSyntheticProgramFramePreview(
  renderPlan: NativeMediaCoreRenderPlan,
  frames: NativeMediaCoreFrame[],
  programFrame: NativeMediaCoreProgramFrame
): ProgramFramePreviewPixels | undefined {
  const sourceWidth = programFrame.width > 0 ? programFrame.width : renderPlan.outputProfile.width;
  const sourceHeight = programFrame.height > 0 ? programFrame.height : renderPlan.outputProfile.height;
  const { width: previewWidth, height: previewHeight } = computeProgramFramePreviewSize(sourceWidth, sourceHeight);
  if (previewWidth <= 0 || previewHeight <= 0) {
    return undefined;
  }

  const bgra = new Uint8Array(previewWidth * previewHeight * 4);
  const background = 0xff0c1118;
  for (let y = 0; y < previewHeight; y += 1) {
    for (let x = 0; x < previewWidth; x += 1) {
      setPixelBgra(bgra, previewWidth, x, y, background);
    }
  }

  if (renderPlan.layers.length > 0) {
    let videoIndex = 0;
    for (const layer of renderPlan.layers) {
      const renderLayer = layer as RenderPlanLayer;
      let rect = renderLayer.rect ?? { x: 0, y: 0, width: 0, height: 0 };
      if (rect.width <= 0 || rect.height <= 0) {
        if (layer.kind === "overlay") {
          rect = layer.layerId.includes("lower") ? lowerThirdOverlay() : topRightOverlay();
        } else {
          const videoLayerCount = renderPlan.layers.filter((entry) => entry.kind !== "overlay").length;
          rect = gridCell(Math.max(1, videoLayerCount), videoIndex);
          videoIndex += 1;
        }
      }

      let color = 0xff2a3548;
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

export function encodeBgraBase64(bgra: Uint8Array): string {
  return Buffer.from(bgra).toString("base64");
}

export function programFramePreviewWire(
  programFrame: NativeMediaCoreProgramFrame,
  preview: ProgramFramePreviewPixels,
  renderer = "software"
): ProgramFramePreviewWire {
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

export function programFramePreviewEvent(
  programFrame: NativeMediaCoreProgramFrame,
  preview: ProgramFramePreviewPixels,
  renderer = "software"
): { type: "program-frame-preview"; preview: ProgramFramePreviewWire } | undefined {
  if (preview.width <= 0 || preview.height <= 0 || preview.bgra.length === 0) {
    return undefined;
  }
  return {
    type: "program-frame-preview",
    preview: programFramePreviewWire(programFrame, preview, renderer)
  };
}

export function programFramePreviewEventFromSnapshot(snapshot: {
  programFrame?: NativeMediaCoreProgramFrame;
  frames: NativeMediaCoreFrame[];
  renderPlan: NativeMediaCoreRenderPlan;
}): { type: "program-frame-preview"; preview: ProgramFramePreviewWire } | undefined {
  if (!snapshot.programFrame) {
    return undefined;
  }
  const preview = fillSyntheticProgramFramePreview(snapshot.renderPlan, snapshot.frames, snapshot.programFrame);
  if (!preview) {
    return undefined;
  }
  return programFramePreviewEvent(snapshot.programFrame, preview);
}