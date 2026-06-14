import type { OutputHealth, VirtualCameraState } from "../domain/production";

export type VirtualCameraStatus = {
  active: boolean;
  label: string;
  outputFormat: string;
  summary: string;
};

/**
 * Describe the virtual camera output that publishes the composited program as a
 * system webcam for use in Zoom, Meet, Teams, or any other capture app. The
 * published format tracks the program output resolution/fps, with an optional
 * mirror for talent-facing comfort.
 */
export function describeVirtualCamera(state: VirtualCameraState, output: OutputHealth): VirtualCameraStatus {
  const deviceName = state.deviceName.trim() || "CoreVideo Pro Camera";
  const outputFormat = `${output.resolution} ${output.fps}fps${state.mirrored ? " mirrored" : ""}`;

  return {
    active: state.enabled,
    label: state.enabled ? `Publishing program to ${deviceName}` : "Virtual camera offline",
    outputFormat,
    summary: state.enabled ? `${deviceName} - ${outputFormat}` : `${deviceName} - idle`
  };
}

export function toggleVirtualCamera(state: VirtualCameraState): VirtualCameraState {
  return { ...state, enabled: !state.enabled };
}
