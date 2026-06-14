export type ControlAction = "take" | "preview" | "magic-scene" | "set-and-forget" | "record" | "stream";

export type ControlButton = {
  id: string;
  label: string;
  action: ControlAction;
};

export const controlSurface: ControlButton[] = [
  { id: "btn-take", label: "Take", action: "take" },
  { id: "btn-preview", label: "Preview", action: "preview" },
  { id: "btn-magic", label: "Magic Scene", action: "magic-scene" },
  { id: "btn-auto", label: "Set & Forget", action: "set-and-forget" },
  { id: "btn-record", label: "Record", action: "record" },
  { id: "btn-stream", label: "Stream", action: "stream" }
];

export type ControlSurfaceContext = {
  recording: boolean;
  streaming: boolean;
  automation: boolean;
  inMeeting: boolean;
};

export type ControlButtonStatus = {
  active: boolean;
  disabled: boolean;
  stateLabel: string;
};

export function resolveControlAction(buttonId: string): ControlAction | undefined {
  return controlSurface.find((button) => button.id === buttonId)?.action;
}

/**
 * Resolve the live state of a control-surface button so an attached Stream Deck
 * or Companion surface can light it correctly: active (lit), disabled (dimmed),
 * and a short state label for the button's secondary text.
 */
export function controlButtonStatus(action: ControlAction, context: ControlSurfaceContext): ControlButtonStatus {
  switch (action) {
    case "record":
      return { active: context.recording, disabled: false, stateLabel: context.recording ? "Live" : "Armed" };
    case "stream":
      return { active: context.streaming, disabled: false, stateLabel: context.streaming ? "On air" : "Ready" };
    case "set-and-forget":
      return { active: context.automation, disabled: false, stateLabel: context.automation ? "On" : "Off" };
    case "magic-scene":
      return { active: false, disabled: !context.inMeeting, stateLabel: context.inMeeting ? "Rebuild" : "Offline" };
    case "preview":
      return { active: false, disabled: false, stateLabel: "Toggle" };
    case "take":
    default:
      return { active: false, disabled: false, stateLabel: "Cut" };
  }
}

export function summarizeControlSurface(): string {
  return `${controlSurface.length} buttons mapped - Stream Deck / Companion ready`;
}
