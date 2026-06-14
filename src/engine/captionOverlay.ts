import type { CaptionOverlayState, SceneTemplate } from "../domain/production";
import type { CaptionOverlayRequest, ZoomSessionSnapshot } from "./contracts";

export class SimulatedCaptionOverlayEngine {
  async buildOverlay(request: CaptionOverlayRequest): Promise<CaptionOverlayState> {
    return buildCaptionOverlay(request.snapshot, request.activeScene);
  }
}

export function buildCaptionOverlay(snapshot: ZoomSessionSnapshot, activeScene: SceneTemplate): CaptionOverlayState {
  const activeSpeaker = snapshot.participants.find((participant) => participant.isActiveSpeaker);
  const captionText = normalizeCaption(snapshot.caption);
  const captionPosition = activeScene.layout === "host-focus" || activeScene.layout === "outro" ? "top" : "bottom";
  const lowerThirdPosition = captionPosition === "bottom" ? "upper-left" : "lower-left";
  const warnings = buildWarnings(captionText, activeScene, snapshot.screenShareActive);
  const confidence = Math.max(82, 97 - Math.floor(captionText.length / 28) - warnings.length * 3);

  return Object.freeze({
    text: captionText,
    speakerName: activeSpeaker?.name ?? snapshot.activeSpeakerName ?? "Program audio",
    confidence,
    latencyMs: activeScene.layout === "smart-grid" ? 240 : 180,
    captionPosition,
    lowerThirdPosition,
    warnings,
    adaptiveSummary: buildAdaptiveSummary(captionPosition, lowerThirdPosition, warnings)
  });
}

function normalizeCaption(caption: string) {
  const trimmed = caption.trim();
  return trimmed.length > 0 ? trimmed : "Captions are standing by for the next speaker.";
}

function buildWarnings(caption: string, activeScene: SceneTemplate, screenShareActive: boolean) {
  const warnings: string[] = [];

  if (caption.length > 96) {
    warnings.push("Caption line is long; compact mode enabled.");
  }

  if (activeScene.layout === "speaker-slides" && screenShareActive) {
    warnings.push("Lower-third lifted to protect slide captions.");
  }

  if (activeScene.layout === "smart-grid") {
    warnings.push("Captions pinned below grid to avoid faces.");
  }

  return warnings;
}

function buildAdaptiveSummary(
  captionPosition: CaptionOverlayState["captionPosition"],
  lowerThirdPosition: CaptionOverlayState["lowerThirdPosition"],
  warnings: string[]
) {
  const base = `Captions ${captionPosition}; lower-third ${lowerThirdPosition.replace("-", " ")}`;
  return warnings.length > 0 ? `${base}; ${warnings[0]}` : base;
}
