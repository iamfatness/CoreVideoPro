import React, { useEffect, useRef, useState } from "react";
import {
  ZOOM_VIDEO_FRAME_STALE_MS,
  zoomVideoFrameStore,
  type ZoomVideoFrame,
  type ZoomVideoFrameDiagnostics
} from "./engine/zoomVideoFrames";

/**
 * Subscribe to the latest live frame for a participant from the global store.
 * Returns `undefined` when no real frame has arrived (mock/in-container runs),
 * so callers can fall back to the simulated placeholder.
 */
export function useZoomVideoFrame(participantId: string): ZoomVideoFrame | undefined {
  const [frame, setFrame] = useState<ZoomVideoFrame | undefined>(() =>
    zoomVideoFrameStore.getLatest(participantId)
  );

  useEffect(() => {
    setFrame(zoomVideoFrameStore.getLatest(participantId));
    return zoomVideoFrameStore.subscribe(participantId, setFrame);
  }, [participantId]);

  return frame;
}

export function useZoomVideoFrameDiagnostics(participantId: string): ZoomVideoFrameDiagnostics {
  const [diagnostics, setDiagnostics] = useState<ZoomVideoFrameDiagnostics>(() =>
    zoomVideoFrameStore.getDiagnostics(participantId)
  );

  useEffect(() => {
    const update = () => setDiagnostics(zoomVideoFrameStore.getDiagnostics(participantId));
    update();
    const unsubscribe = zoomVideoFrameStore.subscribe(participantId, update);
    const interval = window.setInterval(update, ZOOM_VIDEO_FRAME_STALE_MS);
    return () => {
      unsubscribe();
      window.clearInterval(interval);
    };
  }, [participantId]);

  return diagnostics;
}

/**
 * Draws the live RGBA frame for a participant onto a <canvas>. Renders nothing
 * until a real frame arrives, letting the surrounding tile show its placeholder.
 */
export function ParticipantVideoCanvas({ participantId }: { participantId: string }) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const frame = useZoomVideoFrame(participantId);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !frame) {
      return;
    }
    const context = canvas.getContext("2d");
    if (!context) {
      return;
    }
    if (canvas.width !== frame.width || canvas.height !== frame.height) {
      canvas.width = frame.width;
      canvas.height = frame.height;
    }
    const image = context.createImageData(frame.width, frame.height);
    image.data.set(frame.rgba);
    context.putImageData(image, 0, 0);
  }, [frame]);

  if (!frame) {
    return null;
  }

  return <canvas ref={canvasRef} className="video-tile-canvas" aria-hidden="true" data-video-backing="native-frame" />;
}
