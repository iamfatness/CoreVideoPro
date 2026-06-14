import type { PresetSummary, ProductionState, ShowPreset } from "../domain/production";

export class InMemoryPresetEngine {
  private presets = new Map<string, ShowPreset>();

  async savePreset(state: ProductionState): Promise<ShowPreset> {
    const preset = createShowPreset(state);
    this.presets.set(preset.id, preset);
    return preset;
  }

  async loadPreset(id: string): Promise<ShowPreset | undefined> {
    const preset = this.presets.get(id);
    return preset ? clonePreset(preset) : undefined;
  }

  async listPresets(): Promise<PresetSummary[]> {
    return [...this.presets.values()].map((preset) => ({
      id: preset.id,
      name: preset.name,
      savedAt: preset.savedAt,
      sceneCount: preset.scenes.length,
      armedDestinationCount: preset.outputDestinations.filter((destination) => destination.enabled).length,
      enabledGraphicCount: preset.graphics.filter((graphic) => graphic.enabled).length
    }));
  }
}

export function createShowPreset(state: ProductionState): ShowPreset {
  const savedAt = new Date().toISOString();

  return clonePreset({
    id: slugify(state.meetingTitle),
    name: `${state.meetingTitle} Show`,
    savedAt,
    meetingTitle: state.meetingTitle,
    mode: state.mode,
    activeSceneId: state.activeSceneId,
    previewSceneId: state.previewSceneId,
    transition: {
      style: state.transition.style,
      durationMs: state.transition.durationMs
    },
    selectedOutputProfileId: state.selectedOutputProfileId,
    outputProfiles: state.outputProfiles,
    recordingSettings: state.recordingSettings,
    scenes: state.scenes,
    graphics: state.graphics,
    videoEffects: state.videoEffects,
    outputDestinations: state.outputDestinations
  });
}

export function applyShowPreset(state: ProductionState, preset: ShowPreset): ProductionState {
  return {
    ...state,
    meetingTitle: preset.meetingTitle,
    mode: preset.mode,
    activeSceneId: preset.activeSceneId,
    previewSceneId: preset.previewSceneId,
    transition: {
      ...state.transition,
      ...preset.transition,
      statusText: `${preset.name} loaded`
    },
    selectedOutputProfileId: preset.selectedOutputProfileId,
    outputProfiles: preset.outputProfiles.map((profile) => ({ ...profile })),
    recordingSettings: { ...preset.recordingSettings },
    scenes: preset.scenes.map((scene) => ({ ...scene })),
    graphics: preset.graphics.map((graphic) => ({ ...graphic })),
    videoEffects: preset.videoEffects.map((effect) => ({ ...effect })),
    outputDestinations: preset.outputDestinations.map((destination) => ({ ...destination })),
    outputSession: {
      ...state.outputSession,
      activeDestinationIds: [],
      destinations: [],
      streaming: false
    },
    streaming: false
  };
}

function clonePreset(preset: ShowPreset): ShowPreset {
  return {
    ...preset,
    transition: { ...preset.transition },
    outputProfiles: preset.outputProfiles.map((profile) => ({ ...profile })),
    recordingSettings: { ...preset.recordingSettings },
    scenes: preset.scenes.map((scene) => ({
      ...scene,
      slots: scene.slots ? [...scene.slots] : undefined,
      routes: scene.routes ? scene.routes.map((route) => ({ ...route })) : undefined
    })),
    graphics: preset.graphics.map((graphic) => ({ ...graphic })),
    videoEffects: preset.videoEffects.map((effect) => ({ ...effect })),
    outputDestinations: preset.outputDestinations.map((destination) => ({ ...destination }))
  };
}

function slugify(value: string) {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/(^-|-$)/g, "") || "corevideo-show";
}
