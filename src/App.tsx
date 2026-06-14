import {
  Activity,
  AudioLines,
  Bot,
  Cable,
  Captions,
  CircleDot,
  Clapperboard,
  Expand,
  Gauge,
  HardDrive,
  LayoutTemplate,
  LogIn,
  LogOut,
  Maximize2,
  Mic,
  MicOff,
  Minus,
  MonitorUp,
  MoreHorizontal,
  Palette,
  Plus,
  Radio,
  RefreshCw,
  Save,
  Search,
  Settings2,
  Shield,
  Sparkles,
  Square,
  Users,
  Video,
  Volume2,
  Wifi,
  X
} from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { applyBrandKitToGraphics, summarizeBrandKit } from "./engine/brandKit";
import { getFrameForParticipant } from "./engine/mediaFrames";
import { runOutputPreflight, runRecordingPreflight } from "./engine/outputPreflight";
import { applyShowPreset as applyShowPresetState } from "./engine/presets";
import { applyVideoEffectToFrame, getVideoEffect, toggleChromaKey, toggleCropMode } from "./engine/videoEffects";
import {
  getBreakoutRooms,
  initialProduction,
  sortParticipantsForProduction,
  type AutoProductionState,
  type BrandKit,
  type BrandKitFont,
  type GraphicOverlay,
  type MediaFrameState,
  type OutputDestination,
  type OutputProfile,
  type Participant,
  type ParticipantRole,
  type ParticipantVideoEffect,
  type PresetSummary,
  type ProductionState,
  type RecordingSettings,
  type SceneTemplate,
  type SourceRoute,
  type SupportBundle,
  type TransitionState
} from "./domain/production";
import type { MeetingState, ZoomSessionSnapshot } from "./engine/contracts";
import type { EngineBundle } from "./engine/engineBundle";
import type { RuntimeEnvironment } from "./engine/runtimeEnvironment";

const healthLabels: Record<Participant["health"], string> = {
  live: "Live",
  "low-resolution": "Low res",
  recovering: "Recovering",
  "video-off": "Video off"
};

const commandLabels = {
  t: "Take",
  r: "Record",
  s: "Stream",
  m: "Magic Scene",
  f: "Refresh feeds",
  p: "Preview Monitor"
} as const;

type CommandKey = keyof typeof commandLabels;

const participantRoles: ParticipantRole[] = ["Host", "Presenter", "Panelist", "Guest"];
const exclusiveParticipantRoles = new Set<ParticipantRole>(["Host", "Presenter"]);
const brandKitFonts: BrandKitFont[] = ["Inter", "Poppins", "Roboto", "Georgia"];
const lowerThirdStyles: BrandKit["lowerThirdStyle"][] = ["solid", "minimal", "gradient"];

const tabs = [
  { id: "studio", label: "Studio", icon: Clapperboard },
  { id: "settings", label: "Settings", icon: Settings2 },
  { id: "sources", label: "Sources", icon: Cable },
  { id: "overlays", label: "Overlays", icon: Palette },
  { id: "audio", label: "Audio", icon: AudioLines },
  { id: "media", label: "Media", icon: Video },
  { id: "automation", label: "Automation", icon: Bot }
] as const;

type TabId = (typeof tabs)[number]["id"];

const roleBadgeLabels: Record<ParticipantRole, string> = {
  Host: "HOST",
  Presenter: "SPEAKER",
  Panelist: "PANELIST",
  Guest: "ATTENDEE"
};

const sceneDurations: Record<SceneTemplate["layout"], string> = {
  "host-focus": "8s",
  "two-up": "12s",
  "speaker-slides": "12s",
  "smart-grid": "8s",
  outro: "4s"
};

const avatarPalette = ["#44c1a1", "#7fd9a0", "#f0a85c", "#5b9bd5", "#c792ea", "#ef4f4f"];

function avatarColorFor(participantId: string) {
  let hash = 0;
  for (let index = 0; index < participantId.length; index += 1) {
    hash = (hash * 31 + participantId.charCodeAt(index)) % avatarPalette.length;
  }
  return avatarPalette[Math.abs(hash)];
}

export type AppProps = {
  engines: EngineBundle;
  runtime?: RuntimeEnvironment;
};

export function App({ engines, runtime }: AppProps) {
  const [production, setProduction] = useState(initialProduction);
  const [meetingState, setMeetingState] = useState<MeetingState>("in_meeting");
  const [screenShareActive, setScreenShareActive] = useState(true);
  const [elapsedSeconds, setElapsedSeconds] = useState(0);
  const [joinRequest, setJoinRequest] = useState({
    meetingUrl: "https://zoom.us/j/123456789",
    displayName: "CoreVideo Producer",
    webinar: true
  });
  const [joinStatus, setJoinStatus] = useState("Ready");
  const [outputPreflightStatus, setOutputPreflightStatus] = useState("Output preflight ready");
  const [recordingPreflightStatus, setRecordingPreflightStatus] = useState("Recording preflight ready");
  const [presetSummaries, setPresetSummaries] = useState<PresetSummary[]>([]);
  const [presetStatus, setPresetStatus] = useState("No presets saved");
  const [supportBundleStatus, setSupportBundleStatus] = useState("Support bundle ready");
  const [supportBundle, setSupportBundle] = useState<SupportBundle | undefined>();
  const [showPreviewMonitor, setShowPreviewMonitor] = useState(false);
  const [commandStatus, setCommandStatus] = useState("Ready");
  const [participantRoleOverrides, setParticipantRoleOverrides] = useState<Record<string, ParticipantRole>>({});
  const [selectedParticipantId, setSelectedParticipantId] = useState("p2");
  const [activeTab, setActiveTab] = useState<TabId>("studio");
  const [safeAreasEnabled, setSafeAreasEnabled] = useState(false);
  const [expandedParticipantId, setExpandedParticipantId] = useState<string | null>(null);
  const selectedParticipant = useMemo(
    () => production.participants.find((participant) => participant.id === selectedParticipantId),
    [production.participants, selectedParticipantId]
  );
  const selectedAudioMix = useMemo(
    () => production.audioMix.participants.find((mix) => mix.participantId === selectedParticipantId),
    [production.audioMix.participants, selectedParticipantId]
  );
  const selectedVideoEffect = useMemo(
    () => getVideoEffect(production.videoEffects, selectedParticipantId),
    [production.videoEffects, selectedParticipantId]
  );
  const breakoutRooms = useMemo(() => getBreakoutRooms(production.participants), [production.participants]);
  const visibleParticipants = useMemo(
    () =>
      production.selectedBreakoutRoomId === "all"
        ? production.participants
        : production.participants.filter((participant) => participant.breakoutRoomId === production.selectedBreakoutRoomId),
    [production.participants, production.selectedBreakoutRoomId]
  );
  const activeScene = useMemo(
    () => production.scenes.find((scene) => scene.id === production.activeSceneId) ?? production.scenes[0],
    [production.activeSceneId, production.scenes]
  );
  const previewScene = useMemo(
    () => production.scenes.find((scene) => scene.id === production.previewSceneId) ?? activeScene,
    [activeScene, production.previewSceneId, production.scenes]
  );
  const programParticipants = useMemo(
    () => getSceneParticipants(activeScene, visibleParticipants),
    [activeScene, visibleParticipants]
  );
  const previewSceneParticipants = useMemo(
    () => getSceneParticipants(previewScene, visibleParticipants),
    [previewScene, visibleParticipants]
  );
  const previewRouteWarnings = useMemo(
    () => getRouteWarnings(previewScene, visibleParticipants),
    [previewScene, visibleParticipants]
  );
  const activeShareFrame = production.mediaFrames.find(
    (frame) =>
      frame.kind === "screen-share" &&
      visibleParticipants.some((participant) => participant.id === frame.participantId)
  );
  const destinationStates = production.outputDestinations.map((destination) => {
    const sessionState = production.outputSession.destinations.find((item) => item.id === destination.id);
    return sessionState ?? { ...destination, active: false, health: "idle" as const, bitrateMbps: 0 };
  });

  useEffect(() => {
    let mounted = true;

    engines.zoom.getSnapshot().then((snapshot) => {
      if (mounted) {
        applySnapshot(snapshot);
      }
    });
    engines.output.getSession().then((session) => {
      if (mounted) {
        applyOutputSession(session);
      }
    });
    engines.presets.listPresets().then((presets) => {
      if (mounted) {
        setPresetSummaries(presets);
      }
    });
    engines.captureDevices.listDevices().then((captureDevices) => {
      if (mounted) {
        setProduction((current) => ({ ...current, captureDevices }));
      }
    });

    return () => {
      mounted = false;
    };
  }, []);

  async function selectCaptureDeviceInput(deviceId: string, inputId: string) {
    const captureDevices = await engines.captureDevices.selectInput(deviceId, inputId);
    setProduction((current) => ({ ...current, captureDevices }));
  }

  async function setCaptureDeviceAudioSyncOffset(deviceId: string, offsetMs: number) {
    const captureDevices = await engines.captureDevices.setAudioSyncOffset(deviceId, offsetMs);
    setProduction((current) => ({ ...current, captureDevices }));
  }

  async function applySnapshot(snapshot: ZoomSessionSnapshot) {
    const participants = applyParticipantRoleOverrides(snapshot.participants, participantRoleOverrides);
    const snapshotWithOverrides = {
      ...snapshot,
      participants
    };
    const productionWithParticipants = {
      ...production,
      participants
    };
    const scene = production.scenes.find((item) => item.id === production.activeSceneId) ?? production.scenes[0];
    const [audioMix, autoProduction] = await Promise.all([
      engines.audio.buildMix(participants),
      engines.ai.recommendAutoProduction(productionWithParticipants, snapshotWithOverrides)
    ]);
    const overlayScene =
      production.mode === "set-and-forget" && autoProduction.action === "take"
        ? production.scenes.find((item) => item.id === autoProduction.recommendedSceneId) ?? scene
        : scene;
    const captionOverlay = await engines.captions.buildOverlay({ snapshot: snapshotWithOverrides, activeScene: overlayScene });

    setMeetingState(snapshot.meetingState);
    setScreenShareActive(snapshot.screenShareActive);
    setElapsedSeconds(snapshot.elapsedSeconds);
    setProduction((current) => ({
      ...current,
      participants,
      mediaFrames: snapshot.mediaFrames,
      audioMix,
      autoProduction,
      captionOverlay,
      captions: snapshot.caption,
      selectedBreakoutRoomId:
        current.selectedBreakoutRoomId === "all" ||
        participants.some((participant) => participant.breakoutRoomId === current.selectedBreakoutRoomId)
          ? current.selectedBreakoutRoomId
          : "all",
      ...(current.mode === "set-and-forget" ? applyAutoProductionRecommendation(current, autoProduction) : {})
    }));
    setSelectedParticipantId((currentId) => {
      if (participants.some((participant) => participant.id === currentId)) {
        return currentId;
      }

      return participants[0]?.id ?? "";
    });
  }

  function applyOutputSession(session: Awaited<ReturnType<EngineBundle["output"]["getSession"]>>) {
    setProduction((current) => ({
      ...current,
      recording: session.recording,
      streaming: session.streaming,
      output: session.health,
      outputSession: session
    }));
  }

  async function joinMeeting() {
    const request = {
      meetingUrl: joinRequest.meetingUrl.trim(),
      displayName: joinRequest.displayName.trim() || "CoreVideo Producer",
      webinar: joinRequest.webinar
    };

    if (!request.meetingUrl) {
      setJoinStatus("Enter a Zoom meeting URL or ID");
      return;
    }

    setJoinStatus("Joining Zoom...");

    try {
      const snapshot = await engines.zoom.join(request);
      await applySnapshot(snapshot);
      setJoinStatus(`Joined as ${request.displayName}`);
    } catch (error) {
      setJoinStatus(error instanceof Error ? error.message : "Unable to join Zoom");
    }
  }

  async function leaveMeeting() {
    await applySnapshot(await engines.zoom.leave());
  }

  async function refreshFeeds() {
    const snapshot = engines.zoom.advanceSimulation
      ? await engines.zoom.advanceSimulation()
      : await engines.zoom.getSnapshot();
    await applySnapshot(snapshot);
  }

  async function runMagicScene() {
    const result = await engines.ai.buildMagicScene({
      participants: production.participants,
      currentScenes: production.scenes,
      screenShareActive
    });
    const selectedScene = result.scenes.find((scene) => scene.selected) ?? result.scenes[0];

    setProduction((current) => ({
      ...current,
      previewSceneId: selectedScene.id,
      transition: {
        ...current.transition,
        statusText: `${selectedScene.name} queued by Magic Scene`
      },
      magicSceneStatus: result.warnings.length > 0 ? `${result.summary} ${result.warnings.join(" ")}` : result.summary,
      scenes: result.scenes
    }));
  }

  function selectScene(scene: SceneTemplate) {
    setProduction((current) => ({
      ...current,
      previewSceneId: scene.id,
      transition: {
        ...current.transition,
        statusText: scene.id === current.activeSceneId ? "Preview matches program" : `${scene.name} queued`
      },
      scenes: current.scenes.map((item) => ({ ...item, selected: item.id === scene.id }))
    }));
  }

  function updatePreviewSceneRoute(slotIndex: number, update: Partial<SourceRoute>) {
    setProduction((current) => ({
      ...current,
      scenes: current.scenes.map((scene) => {
        if (scene.id !== current.previewSceneId) {
          return scene;
        }

        const nextRoutes = getRouteDefaults(scene, current.participants);
        const currentRoute = nextRoutes[slotIndex];
        const nextRoute = normalizeRouteUpdate({ ...currentRoute, ...update }, current.participants);
        nextRoutes[slotIndex] = nextRoute;
        return {
          ...scene,
          routes: nextRoutes,
          slots: nextRoutes.map(routeToSlot).filter(Boolean) as string[]
        };
      }),
      transition: {
        ...current.transition,
        statusText: `${previewScene.name} route ${slotIndex + 1} updated`
      }
    }));
  }

  function assignPreviewSceneSlot(slotIndex: number, participantId: string) {
    updatePreviewSceneRoute(slotIndex, { mode: "fixed", participantId, audioRole: "isolated" });
  }

  async function takePreviewScene() {
    const scene = production.scenes.find((item) => item.id === production.previewSceneId) ?? activeScene;
    const captionOverlay = await engines.captions.buildOverlay({
      snapshot: buildCurrentSnapshot(),
      activeScene: scene
    });

    setProduction((current) => ({
      ...current,
      activeSceneId: scene.id,
      previewSceneId: scene.id,
      captionOverlay,
      transition: {
        ...current.transition,
        lastTakenSceneId: scene.id,
        statusText: `${scene.name} taken with ${current.transition.style}`
      },
      scenes: current.scenes.map((item) => ({ ...item, selected: item.id === scene.id }))
    }));
  }

  function setTransitionStyle(style: TransitionState["style"]) {
    const durations: Record<TransitionState["style"], number> = {
      cut: 0,
      fade: 420,
      slide: 520
    };

    setProduction((current) => ({
      ...current,
      transition: {
        ...current.transition,
        style,
        durationMs: durations[style],
        statusText: `${style} transition selected`
      }
    }));
  }

  function buildCurrentSnapshot(): ZoomSessionSnapshot {
    return {
      meetingState,
      participants: production.participants,
      mediaFrames: production.mediaFrames,
      activeSpeakerName: production.participants.find((participant) => participant.isActiveSpeaker)?.name ?? "",
      screenShareActive,
      caption: production.captions,
      elapsedSeconds
    };
  }

  function toggleAutomation() {
    setProduction((current) => ({
      ...current,
      mode: current.mode === "set-and-forget" ? "manual" : "set-and-forget",
      transition: {
        ...current.transition,
        statusText:
          current.mode === "set-and-forget"
            ? "Manual control enabled"
            : `Set & Forget enabled: ${current.autoProduction.reason}`
      }
    }));
  }

  async function toggleRecording() {
    if (production.recording) {
      applyOutputSession(await engines.output.stopRecording());
      setRecordingPreflightStatus("Recording stopped");
    } else {
      const preflight = runRecordingPreflight(production.recordingSettings);
      setRecordingPreflightStatus(preflight.errors.length > 0 ? preflight.errors.join(" ") : [...preflight.warnings, preflight.summary].join(" "));

      if (!preflight.ready) {
        return;
      }

      applyOutputSession(await engines.output.startRecording(production.recordingSettings));
    }
  }

  function updateRecordingSettings(update: Partial<RecordingSettings>) {
    if (production.recording) {
      return;
    }

    setProduction((current) => ({
      ...current,
      recordingSettings: {
        ...current.recordingSettings,
        ...update
      }
    }));
  }

  function toggleIsoRecording(participantId: string) {
    if (production.recording) {
      return;
    }

    setProduction((current) => {
      const selected = current.recordingSettings.isoParticipantIds.includes(participantId);
      return {
        ...current,
        recordingSettings: {
          ...current.recordingSettings,
          isoParticipantIds: selected
            ? current.recordingSettings.isoParticipantIds.filter((id) => id !== participantId)
            : [...current.recordingSettings.isoParticipantIds, participantId]
        }
      };
    });
  }

  async function toggleStreaming() {
    if (production.streaming) {
      applyOutputSession(await engines.output.stopStream());
      setOutputPreflightStatus("Streaming stopped");
    } else {
      const preflight = runOutputPreflight(production.outputDestinations);
      setOutputPreflightStatus(preflight.errors.length > 0 ? preflight.errors.join(" ") : [...preflight.warnings, preflight.summary].join(" "));

      if (!preflight.ready) {
        return;
      }

      applyOutputSession(await engines.output.startStream(preflight.destinations));
    }
  }

  async function selectOutputProfile(profile: OutputProfile) {
    const session = await engines.output.setOutputProfile(profile);
    setProduction((current) => ({
      ...current,
      selectedOutputProfileId: profile.id,
      output: session.health,
      outputSession: session
    }));
  }

  function toggleOutputDestination(destinationId: string) {
    if (production.streaming) {
      return;
    }

    setProduction((current) => ({
      ...current,
      outputDestinations: current.outputDestinations.map((destination) =>
        destination.id === destinationId ? { ...destination, enabled: !destination.enabled } : destination
      )
    }));
  }

  function updateOutputDestination(destinationId: string, update: Partial<Pick<OutputDestination, "endpoint" | "streamKey">>) {
    if (production.streaming) {
      return;
    }

    setProduction((current) => ({
      ...current,
      outputDestinations: current.outputDestinations.map((destination) =>
        destination.id === destinationId ? { ...destination, ...update } : destination
      )
    }));
  }

  function toggleGraphic(graphicId: string) {
    setProduction((current) => ({
      ...current,
      graphics: current.graphics.map((graphic) =>
        graphic.id === graphicId ? { ...graphic, enabled: !graphic.enabled } : graphic
      )
    }));
  }

  function updateBrandKit(update: Partial<ProductionState["brandKit"]>) {
    setProduction((current) => ({
      ...current,
      brandKit: { ...current.brandKit, ...update }
    }));
  }

  function applyBrandKit() {
    setProduction((current) => ({
      ...current,
      graphics: applyBrandKitToGraphics(current.graphics, current.brandKit)
    }));
  }

  function selectBreakoutRoom(roomId: string) {
    const nextParticipants =
      roomId === "all"
        ? production.participants
        : production.participants.filter((participant) => participant.breakoutRoomId === roomId);

    setProduction((current) => ({
      ...current,
      selectedBreakoutRoomId: roomId
    }));
    setSelectedParticipantId((currentId) =>
      nextParticipants.some((participant) => participant.id === currentId)
        ? currentId
        : nextParticipants[0]?.id ?? production.participants[0]?.id ?? ""
    );
  }

  function toggleSelectedChromaKey() {
    if (!selectedParticipant) {
      return;
    }

    setProduction((current) => ({
      ...current,
      videoEffects: toggleChromaKey(current.videoEffects, selectedParticipant.id)
    }));
  }

  function toggleSelectedCropMode() {
    if (!selectedParticipant) {
      return;
    }

    setProduction((current) => ({
      ...current,
      videoEffects: toggleCropMode(current.videoEffects, selectedParticipant.id)
    }));
  }

  async function savePreset() {
    const preset = await engines.presets.savePreset(production);
    const summaries = await engines.presets.listPresets();
    setPresetSummaries(summaries);
    setPresetStatus(`${preset.name} saved`);
  }

  async function loadPreset(presetId: string) {
    const preset = await engines.presets.loadPreset(presetId);
    if (!preset) {
      setPresetStatus("Preset unavailable");
      return;
    }

    setProduction((current) => applyShowPresetState(current, preset));
    setPresetStatus(`${preset.name} loaded`);
  }

  async function exportSupportBundle() {
    const bundle = await engines.diagnostics.createSupportBundle(production);
    setSupportBundle(bundle);
    setSupportBundleStatus(`${bundle.id} exported`);
  }

  async function toggleSelectedParticipantMute() {
    if (!selectedParticipant) {
      return;
    }

    const mix = await engines.audio.setParticipantMuted(
      selectedParticipant.id,
      !(selectedAudioMix?.muted ?? selectedParticipant.isMuted),
      production.participants
    );
    setProduction((current) => ({ ...current, audioMix: mix }));
  }

  async function setSelectedParticipantGain(gainDb: number) {
    if (!selectedParticipant) {
      return;
    }

    const mix = await engines.audio.setParticipantGain(selectedParticipant.id, gainDb, production.participants);
    setProduction((current) => ({ ...current, audioMix: mix }));
  }

  function setSelectedParticipantRole(role: ParticipantRole) {
    if (!selectedParticipant) {
      return;
    }

    const selectedId = selectedParticipant.id;

    setParticipantRoleOverrides((current) => {
      const next = { ...current, [selectedId]: role };

      if (exclusiveParticipantRoles.has(role)) {
        production.participants.forEach((participant) => {
          if (participant.id !== selectedId && participant.role === role) {
            next[participant.id] = "Guest";
          }
        });
      }

      return next;
    });
    setProduction((current) => ({
      ...current,
      participants: current.participants.map((participant) => {
        if (participant.id === selectedId) {
          return { ...participant, role };
        }

        if (exclusiveParticipantRoles.has(role) && participant.role === role) {
          return { ...participant, role: "Guest" };
        }

        return participant;
      }),
      magicSceneStatus: `${selectedParticipant.name} set as ${role} for scene automation`,
      transition: {
        ...current.transition,
        statusText: `${selectedParticipant.name} role set to ${role}`
      }
    }));
  }

  async function runCommand(command: CommandKey) {
    setCommandStatus(commandLabels[command]);

    switch (command) {
      case "t":
        await takePreviewScene();
        break;
      case "r":
        await toggleRecording();
        break;
      case "s":
        await toggleStreaming();
        break;
      case "m":
        if (meetingState === "in_meeting") {
          await runMagicScene();
        }
        break;
      case "f":
        if (meetingState === "in_meeting") {
          await refreshFeeds();
        }
        break;
      case "p":
        setShowPreviewMonitor((current) => !current);
        break;
    }
  }

  useEffect(() => {
    function handleKeyDown(event: KeyboardEvent) {
      if (event.repeat || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey || isEditableTarget(event.target)) {
        return;
      }

      const command = event.key.toLowerCase() as CommandKey;

      if (!(command in commandLabels)) {
        return;
      }

      event.preventDefault();
      void runCommand(command);
    }

    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  });

  const outputProfileBadge = production.outputProfiles.find((profile) => profile.id === production.selectedOutputProfileId);
  const cpuLoad = production.output.encoderLoad;
  const memoryLoad = Math.min(100, Math.round(production.output.encoderLoad * 0.7 + 10));
  const diskLoad = Math.min(100, Math.round(production.output.encoderLoad * 0.4 + 20));
  const masterLevel = production.audioMix.masterLevel;

  return (
    <div className="app-shell">
      <header className="topbar">
        <div className="brand">
          <div className="brand-mark">CV</div>
          <div>
            <strong>CoreVideo Pro</strong>
            <span>Zoom-native production</span>
          </div>
        </div>

        <nav className="topbar-nav" aria-label="Primary navigation">
          {tabs.map((tab) => {
            const Icon = tab.icon;
            return (
              <button
                className={`topbar-nav-item ${activeTab === tab.id ? "active" : ""}`}
                key={tab.id}
                onClick={() => setActiveTab(tab.id)}
              >
                <Icon size={15} />
                {tab.label}
              </button>
            );
          })}
        </nav>

        <div className="topbar-right">
          <div className={`connection-pill ${meetingState === "in_meeting" ? "connected" : ""}`}>
            <span className="connection-dot" />
            {meetingState === "in_meeting" ? "Zoom Connected" : "Zoom Offline"}
          </div>
          <Wifi size={16} className="decorative-icon" aria-hidden="true" />
          <button className="window-icon" type="button" aria-hidden="true" tabIndex={-1}>
            <Minus size={14} />
          </button>
          <button className="window-icon" type="button" aria-hidden="true" tabIndex={-1}>
            <Square size={12} />
          </button>
          <button className="window-icon" type="button" aria-hidden="true" tabIndex={-1}>
            <X size={14} />
          </button>
        </div>
      </header>

      <div aria-label="Studio tab" className={`tab-content ${activeTab === "studio" ? "active" : ""}`} hidden={activeTab !== "studio"}>
        <div className="studio-grid">
          <aside className="scene-column" aria-label="Scenes">
            <div className="column-header">
              <span>Scenes</span>
              <button className="icon-button" onClick={() => undefined} aria-label="Add scene">
                <Plus size={14} />
              </button>
            </div>
            <div className="scene-list">
              {production.scenes.map((scene) => {
                const SceneIcon = sceneLayoutIcon(scene.layout);
                return (
                  <button
                    className={`scene-item ${scene.id === production.activeSceneId ? "program" : ""} ${scene.id === production.previewSceneId ? "selected" : ""}`}
                    key={scene.id}
                    onClick={() => selectScene(scene)}
                  >
                    <SceneIcon size={16} />
                    <span>{scene.name}</span>
                    <small>{scene.automation}</small>
                    <div className="scene-item-footer">
                      <em className="scene-duration">{sceneDurations[scene.layout]}</em>
                      <em className="scene-status">{scene.id === production.activeSceneId ? "Program" : scene.id === production.previewSceneId ? "Preview" : ""}</em>
                    </div>
                  </button>
                );
              })}
            </div>
            <button className="ghost-button wide" onClick={() => undefined}>
              <Activity size={16} />
              Reorder Scenes
            </button>
          </aside>

          <section className="program-column">
            <div className="program-toolbar">
              <span className="program-label">PROGRAM</span>
              <span className="badge">{outputProfileBadge?.resolution ?? production.output.resolution}{outputProfileBadge ? outputProfileBadge.fps : production.output.fps}</span>
              <span className="badge">16:9</span>
              <div className="program-toolbar-actions">
                <button
                  className={`ghost-button small ${safeAreasEnabled ? "selected" : ""}`}
                  onClick={() => setSafeAreasEnabled((current) => !current)}
                >
                  <Shield size={14} />
                  Safe Areas
                </button>
                <button
                  className={`ghost-button small ${showPreviewMonitor ? "selected" : ""}`}
                  onClick={() => {
                    setCommandStatus(commandLabels.p);
                    setShowPreviewMonitor((current) => !current);
                  }}
                >
                  <Maximize2 size={14} />
                  Preview Monitor
                </button>
              </div>
            </div>

            <section className="program-frame" aria-label="Program preview">
              <div className={`program-canvas layout-${activeScene.layout} ${safeAreasEnabled ? "safe-areas" : ""}`}>
                <ScenePreview
                  activeShareFrame={activeShareFrame}
                  participants={programParticipants}
                  scene={activeScene}
                  frames={production.mediaFrames}
                  videoEffects={production.videoEffects}
                />
                {(production.recording || production.streaming) && (
                  <div className="live-badge">
                    <span className="live-dot" />
                    LIVE
                  </div>
                )}
                <div className={`lower-third position-${production.captionOverlay.lowerThirdPosition}`}>
                  <strong>{selectedParticipant?.name ?? "CoreVideo Pro"}</strong>
                  <span>{selectedParticipant?.title ?? "Zoom-native production"}</span>
                </div>
                {production.captionOverlay.warnings.length > 0 && (
                  <div className="overlay-warning">{production.captionOverlay.warnings[0]}</div>
                )}
                {production.graphics.filter((graphic) => graphic.enabled).map((graphic) => (
                  <ProgramGraphic key={graphic.id} graphic={graphic} />
                ))}
              </div>
              <div className="caption-strip-row">
                <span className="cc-badge">
                  <Captions size={14} />
                  CC
                </span>
                <div className="caption-strip-text">
                  <strong>{production.captionOverlay.speakerName}</strong>
                  <span>{production.captionOverlay.text}</span>
                </div>
              </div>
            </section>

            {showPreviewMonitor && (
              <section className="preview-frame" aria-label="Preview monitor">
                <div className="program-header preview-header">
                  <span className="preview-dot" />
                  Preview
                  <strong>{previewScene.name}</strong>
                  <small>{production.transition.style} {production.transition.durationMs}ms</small>
                </div>
                <div className={`program-canvas preview-canvas layout-${previewScene.layout}`}>
                  <ScenePreview
                    activeShareFrame={activeShareFrame}
                    participants={previewSceneParticipants}
                    scene={previewScene}
                    frames={production.mediaFrames}
                    videoEffects={production.videoEffects}
                  />
                </div>
              </section>
            )}
          </section>

          <aside className="participants-column" aria-label="Participants and source controls">
            <div className="column-header">
              <span>Participants ({visibleParticipants.length})</span>
              <div className="column-header-actions">
                <Search size={14} className="decorative-icon" aria-hidden="true" />
                <MoreHorizontal size={14} className="decorative-icon" aria-hidden="true" />
              </div>
            </div>

            <div className="breakout-list" aria-label="Breakout rooms">
              <button
                className={production.selectedBreakoutRoomId === "all" ? "selected" : ""}
                onClick={() => selectBreakoutRoom("all")}
              >
                <strong>All rooms</strong>
                <span>{production.participants.length} participants</span>
              </button>
              {breakoutRooms.map((room) => (
                <button
                  className={production.selectedBreakoutRoomId === room.id ? "selected" : ""}
                  key={room.id}
                  onClick={() => selectBreakoutRoom(room.id)}
                >
                  <strong>{room.name}</strong>
                  <span>{room.participantCount} participants</span>
                </button>
              ))}
            </div>

            <h2 className="visually-hidden">Zoom participants</h2>
            <div className="participant-list">
              {visibleParticipants.map((participant) => {
                const mix = production.audioMix.participants.find((item) => item.participantId === participant.id);
                const muted = mix?.muted ?? participant.isMuted;
                const level = mix?.outputLevel ?? participant.audioLevel;
                const initials = participant.name.split(" ").map((part) => part[0]).join("");
                const isExpanded = expandedParticipantId === participant.id;

                return (
                  <div
                    className={`participant-card ${participant.id === selectedParticipantId ? "selected" : ""} ${participant.isActiveSpeaker ? "talking" : ""}`}
                    key={participant.id}
                  >
                    <button
                      className="participant-row"
                      onClick={() => setSelectedParticipantId(participant.id)}
                    >
                      <div className="avatar" style={{ "--avatar-accent": avatarColorFor(participant.id) } as React.CSSProperties}>
                        {initials}
                      </div>
                      <div className="participant-main">
                        <strong>{participant.name}</strong>
                        <span>
                          {participant.role} - {participant.breakoutRoomName} - {healthLabels[participant.health]}
                        </span>
                        {participant.isActiveSpeaker && <em className="talking-indicator">Talking</em>}
                      </div>
                      <span className={`role-badge role-${participant.role.toLowerCase()}`}>{roleBadgeLabels[participant.role]}</span>
                      {muted ? <MicOff size={16} className="mic-icon muted" /> : <Mic size={16} className="mic-icon unmuted" />}
                      <meter min={0} max={100} value={level} />
                      <span
                        className="icon-button expand-toggle"
                        role="button"
                        tabIndex={-1}
                        aria-hidden="true"
                        onClick={(event) => {
                          event.stopPropagation();
                          setExpandedParticipantId((current) => (current === participant.id ? null : participant.id));
                        }}
                      >
                        <Expand size={14} />
                      </span>
                    </button>
                    {isExpanded && participant.id === selectedParticipantId && selectedParticipant && (
                      <SmartHandlingPanel
                        production={production}
                        selectedAudioMix={selectedAudioMix}
                        selectedParticipant={selectedParticipant}
                        selectedVideoEffect={selectedVideoEffect}
                        setSelectedParticipantGain={setSelectedParticipantGain}
                        setSelectedParticipantRole={setSelectedParticipantRole}
                        toggleSelectedChromaKey={toggleSelectedChromaKey}
                        toggleSelectedCropMode={toggleSelectedCropMode}
                        toggleSelectedParticipantMute={toggleSelectedParticipantMute}
                      />
                    )}
                  </div>
                );
              })}
            </div>

            <div className="participants-footer">
              <button className="ghost-button wide" onClick={() => undefined}>
                <Plus size={16} />
                Add Participant
              </button>
              <button className="icon-button" aria-label="More participant actions" onClick={() => undefined}>
                <MoreHorizontal size={16} />
              </button>
            </div>
          </aside>
        </div>
      </div>

      <div aria-label="Settings tab" className={`tab-content ${activeTab === "settings" ? "active" : ""}`} hidden={activeTab !== "settings"}>
        <div className="tab-panel">
          <section className="panel">
            <div className="section-title">
              <Activity size={15} />
              Zoom connection
            </div>
            <div className="zoom-connect" aria-label="Zoom connection">
              <input
                aria-label="Zoom meeting URL or ID"
                disabled={meetingState === "in_meeting"}
                onChange={(event) => setJoinRequest((current) => ({ ...current, meetingUrl: event.target.value }))}
                value={joinRequest.meetingUrl}
              />
              <input
                aria-label="Producer display name"
                disabled={meetingState === "in_meeting"}
                onChange={(event) => setJoinRequest((current) => ({ ...current, displayName: event.target.value }))}
                value={joinRequest.displayName}
              />
              <label className="webinar-toggle">
                <input
                  checked={joinRequest.webinar}
                  disabled={meetingState === "in_meeting"}
                  onChange={(event) => setJoinRequest((current) => ({ ...current, webinar: event.target.checked }))}
                  type="checkbox"
                />
                Webinar
              </label>
              <span>{joinStatus}</span>
            </div>
            <p className="topbar-status-line">
              {meetingState.replace("_", " ")} - {production.participants.length} participants -{" "}
              {production.selectedBreakoutRoomId === "all" ? "All rooms" : breakoutRooms.find((room) => room.id === production.selectedBreakoutRoomId)?.name} -{" "}
              {screenShareActive ? "Screen share active" : "No screen share"} - Captions on - {production.output.resolution} {production.output.fps}fps -{" "}
              {formatElapsed(elapsedSeconds)}
            </p>
            <div className="settings-actions">
              {runtime && (
                <div className={`runtime-badge runtime-${runtime.status}`} aria-label="Desktop runtime">
                  <strong>{runtime.label}</strong>
                  <span>
                    {runtime.host} / {runtime.platform}
                  </span>
                </div>
              )}
              {meetingState === "in_meeting" ? (
                <button className="ghost-button" onClick={leaveMeeting}>
                  <LogOut size={16} />
                  Leave
                </button>
              ) : (
                <button className="ghost-button" onClick={joinMeeting}>
                  <LogIn size={16} />
                  Join Zoom
                </button>
              )}
              <button className="ghost-button" onClick={refreshFeeds} disabled={meetingState !== "in_meeting"}>
                <RefreshCw size={16} />
                Refresh feeds
              </button>
            </div>
          </section>

          <section className="panel">
            <div className="section-title">
              <CircleDot size={15} />
              Recording
            </div>
            <div className="recording-settings" aria-label="Recording settings">
              <label>
                <span>Folder</span>
                <input
                  aria-label="Recording folder"
                  disabled={production.recording}
                  onChange={(event) => updateRecordingSettings({ folder: event.target.value })}
                  value={production.recordingSettings.folder}
                />
              </label>
              <label>
                <span>Filename</span>
                <input
                  aria-label="Recording filename prefix"
                  disabled={production.recording}
                  onChange={(event) => updateRecordingSettings({ filenamePrefix: event.target.value })}
                  value={production.recordingSettings.filenamePrefix}
                />
              </label>
              <label>
                <span>Format</span>
                <select
                  aria-label="Recording format"
                  disabled={production.recording}
                  onChange={(event) => updateRecordingSettings({ format: event.target.value as RecordingSettings["format"] })}
                  value={production.recordingSettings.format}
                >
                  <option value="mp4">MP4</option>
                  <option value="mov">MOV</option>
                  <option value="mkv">MKV</option>
                </select>
              </label>
              <label>
                <span>Quality</span>
                <select
                  aria-label="Recording quality"
                  disabled={production.recording}
                  onChange={(event) => updateRecordingSettings({ quality: event.target.value as RecordingSettings["quality"] })}
                  value={production.recordingSettings.quality}
                >
                  <option value="standard">Standard</option>
                  <option value="high">High</option>
                  <option value="archive">Archive</option>
                </select>
              </label>
              <div className="iso-selector" aria-label="ISO recording feeds">
                <span>ISO feeds</span>
                {production.participants.map((participant) => (
                  <label key={participant.id}>
                    <input
                      aria-label={`${participant.name} ISO recording`}
                      checked={production.recordingSettings.isoParticipantIds.includes(participant.id)}
                      disabled={production.recording || participant.health === "video-off"}
                      onChange={() => toggleIsoRecording(participant.id)}
                      type="checkbox"
                    />
                    <strong>{participant.name}</strong>
                    <em>{participant.role}</em>
                  </label>
                ))}
              </div>
            </div>
          </section>

          <section className="panel">
            <div className="section-title">
              <Radio size={15} />
              Destinations
            </div>
            <div className="destination-list">
              {destinationStates.map((destination) => (
                <div
                  className={`destination-row ${destination.enabled ? "enabled" : ""} ${destination.active ? "active" : ""}`}
                  key={destination.id}
                >
                  <button disabled={production.streaming} onClick={() => toggleOutputDestination(destination.id)}>
                    <div>
                      <strong>{destination.name}</strong>
                      <span>
                        {destination.protocol} - {destination.latencyMs} ms
                      </span>
                    </div>
                    <em>{destination.active ? `${destination.health} ${destination.bitrateMbps} Mbps` : destination.enabled ? "Armed" : "Off"}</em>
                  </button>
                  <div className="destination-settings">
                    <label>
                      <span>{destination.protocol === "NDI" ? "Source name" : "Endpoint"}</span>
                      <input
                        aria-label={`${destination.name} endpoint`}
                        disabled={production.streaming}
                        onChange={(event) => updateOutputDestination(destination.id, { endpoint: event.target.value })}
                        value={destination.endpoint}
                      />
                    </label>
                    {destination.protocol !== "NDI" && (
                      <label>
                        <span>{destination.protocol === "SRT" ? "Passphrase" : "Stream key"}</span>
                        <input
                          aria-label={`${destination.name} stream key`}
                          disabled={production.streaming}
                          onChange={(event) => updateOutputDestination(destination.id, { streamKey: event.target.value })}
                          type="password"
                          value={destination.streamKey ?? ""}
                        />
                      </label>
                    )}
                  </div>
                </div>
              ))}
            </div>
          </section>

          <section className="panel">
            <div className="section-title">
              <MonitorUp size={15} />
              Output profile
            </div>
            <div className="profile-list" aria-label="Output profiles">
              {production.outputProfiles.map((profile) => (
                <button
                  className={production.selectedOutputProfileId === profile.id ? "selected" : ""}
                  disabled={production.recording || production.streaming}
                  key={profile.id}
                  onClick={() => selectOutputProfile(profile)}
                >
                  <strong>{profile.name}</strong>
                  <span>
                    {profile.resolution} - {profile.fps}fps - {profile.targetBitrateMbps} Mbps
                  </span>
                </button>
              ))}
            </div>
          </section>

          <section className="panel">
            <div className="section-title">
              <Activity size={15} />
              Output health
            </div>
            <div className="health-grid">
              <ControlReadout label="Output profile" value={outputProfileBadge?.name ?? "Custom"} />
              <ControlReadout label="Network" value={production.output.network} />
              <ControlReadout label="Frame rate" value={`${production.output.fps} fps`} />
              <ControlReadout label="Audio mix" value={production.audioMix.summary} />
              <ControlReadout label="Loudness" value={`${production.audioMix.loudnessLufs} LUFS`} />
              <ControlReadout label="Caption confidence" value={`${production.captionOverlay.confidence}%`} />
              <ControlReadout label="Caption latency" value={`${production.captionOverlay.latencyMs} ms`} />
              <ControlReadout label="Overlay guard" value={production.captionOverlay.adaptiveSummary} />
              <ControlReadout label="Route health" value={previewRouteWarnings[0] ?? "Ready"} />
              <ControlReadout label="Auto director" value={`${production.autoProduction.action} ${production.autoProduction.confidence}%`} />
              <ControlReadout label="Output time" value={formatElapsed(production.outputSession.elapsedSeconds)} />
              <ControlReadout label="Recording file" value={production.outputSession.recordingFile ? "Active" : "Not recording"} />
              <ControlReadout label="Recording quality" value={production.recordingSettings.quality} />
              <ControlReadout label="Record preflight" value={recordingPreflightStatus} />
              <ControlReadout label="Support bundle" value={supportBundleStatus} />
              <ControlReadout
                label="Stream targets"
                value={production.outputSession.activeDestinationIds.length > 0 ? production.outputSession.activeDestinationIds.join(", ") : "Not streaming"}
              />
              <ControlReadout label="Captions" value="Adaptive realtime" />
              <ControlReadout label="Destination" value={`${getEnabledDestinations(production.outputDestinations).length} armed`} />
              <ControlReadout label="Preflight" value={outputPreflightStatus} />
            </div>
          </section>

          <section className="panel">
            <div className="section-title">
              <Save size={15} />
              Presets
            </div>
            <button className="primary-action" onClick={savePreset}>
              <Save size={16} />
              Save Show
            </button>
            <p className="preset-status">{presetStatus}</p>
            {presetSummaries.length > 0 && (
              <div className="preset-list">
                {presetSummaries.map((preset) => (
                  <button key={preset.id} onClick={() => loadPreset(preset.id)}>
                    <strong>{preset.name}</strong>
                    <span>
                      {preset.sceneCount} scenes - {preset.armedDestinationCount} destinations - {preset.enabledGraphicCount} graphics
                    </span>
                  </button>
                ))}
              </div>
            )}
          </section>

          <section className="panel">
            <div className="section-title">
              <Activity size={15} />
              Diagnostics
            </div>
            <button className="primary-action" onClick={exportSupportBundle}>
              <Save size={16} />
              Export Bundle
            </button>
            <p className="preset-status">{supportBundleStatus}</p>
            {supportBundle && (
              <div className="diagnostics-summary" aria-label="Support bundle summary">
                <strong>{supportBundle.triageLines[0]}</strong>
                <span>{supportBundle.triageLines[3]}</span>
                <span>{supportBundle.triageLines[5]}</span>
                <em>{supportBundle.warnings.length} warnings</em>
              </div>
            )}
          </section>
        </div>
      </div>

      <div aria-label="Sources tab" className={`tab-content ${activeTab === "sources" ? "active" : ""}`} hidden={activeTab !== "sources"}>
        <div className="tab-panel">
          <section className="panel">
            <div className="section-title">
              <LayoutTemplate size={15} />
              Scene slots
            </div>
            <div className="slot-editor" aria-label="Scene slot assignments">
              <strong>{previewScene.name}</strong>
              <span>{previewSceneParticipants.map((participant) => participant.name).join(" + ") || "No participants assigned"}</span>
              {previewRouteWarnings.length > 0 && (
                <div className="route-warnings" aria-label="Route warnings">
                  {previewRouteWarnings.slice(0, 3).map((warning) => (
                    <span key={warning}>{warning}</span>
                  ))}
                </div>
              )}
              {getRouteDefaults(previewScene, visibleParticipants).map((route, index) => (
                <div className="route-editor" key={`${previewScene.id}-${index}`}>
                  <label>
                    <span>Slot {index + 1}</span>
                    <select
                      aria-label={`Slot ${index + 1} route mode`}
                      onChange={(event) => updatePreviewSceneRoute(index, { mode: event.target.value as SourceRoute["mode"] })}
                      value={route.mode}
                    >
                      <option value="fixed">Fixed participant</option>
                      <option value="active-speaker">Active speaker</option>
                      <option value="spotlight">Spotlight</option>
                      <option value="screen-share">Screen share</option>
                      <option value="none">None</option>
                    </select>
                  </label>
                  <label>
                    <span>Source</span>
                    <select
                      aria-label={`Slot ${index + 1} participant`}
                      disabled={route.mode !== "fixed" && route.mode !== "spotlight"}
                      onChange={(event) => assignPreviewSceneSlot(index, event.target.value)}
                      value={route.participantId ?? visibleParticipants[0]?.id ?? ""}
                    >
                      {visibleParticipants.map((participant) => (
                        <option key={participant.id} value={participant.id}>
                          {participant.name}
                        </option>
                      ))}
                    </select>
                  </label>
                  <label>
                    <span>Audio</span>
                    <select
                      aria-label={`Slot ${index + 1} audio role`}
                      onChange={(event) => updatePreviewSceneRoute(index, { audioRole: event.target.value as SourceRoute["audioRole"] })}
                      value={route.audioRole}
                    >
                      <option value="mix">Mix</option>
                      <option value="isolated">Isolated</option>
                      <option value="audience">Audience</option>
                    </select>
                  </label>
                </div>
              ))}
            </div>
          </section>

          <section className="panel" aria-label="Capture devices">
            <div className="section-title">
              <Cable size={15} />
              Capture Devices
            </div>
            {production.captureDevices.length === 0 && <p className="preset-status">No Blackmagic or AJA devices detected</p>}
            {production.captureDevices.map((device) => (
              <div className="capture-device" key={device.id}>
                <div className="capture-device-header">
                  <strong>{device.name}</strong>
                  <span className={`capture-device-status capture-device-status-${device.connectionState}`}>
                    {device.connectionState.replace("-", " ")}
                  </span>
                </div>
                <p className="preset-status">
                  {device.resolution.width}x{device.resolution.height} - {device.frameRate}fps -{" "}
                  {device.signalPresent ? "Signal present" : "No signal"}
                </p>
                <label className="capture-device-field">
                  Input
                  <select
                    aria-label={`${device.name} input`}
                    onChange={(event) => selectCaptureDeviceInput(device.id, event.target.value)}
                    value={device.selectedInputId}
                  >
                    {device.inputs.map((input) => (
                      <option key={input.id} value={input.id}>
                        {input.label}
                      </option>
                    ))}
                  </select>
                </label>
                <label className="capture-device-field">
                  A/V sync offset (ms)
                  <input
                    aria-label={`${device.name} audio sync offset`}
                    max={500}
                    min={-500}
                    onChange={(event) => setCaptureDeviceAudioSyncOffset(device.id, Number(event.target.value))}
                    type="number"
                    value={device.audioSyncOffsetMs}
                  />
                </label>
              </div>
            ))}
          </section>
        </div>
      </div>

      <div aria-label="Overlays tab" className={`tab-content ${activeTab === "overlays" ? "active" : ""}`} hidden={activeTab !== "overlays"}>
        <div className="tab-panel">
          <section className="panel">
            <div className="section-title">
              <Palette size={15} />
              Graphics
            </div>
            <div className="graphics-list">
              {production.graphics.map((graphic) => (
                <button
                  className={`graphic-row ${graphic.enabled ? "enabled" : ""}`}
                  key={graphic.id}
                  onClick={() => toggleGraphic(graphic.id)}
                >
                  <i style={{ "--graphic-accent": graphic.accent } as React.CSSProperties} />
                  <div>
                    <strong>{graphic.name}</strong>
                    <span>
                      {graphic.kind} - {graphic.position.replace("-", " ")}
                    </span>
                  </div>
                  <em>{graphic.enabled ? "On" : "Off"}</em>
                </button>
              ))}
            </div>
          </section>

          <section className="panel" aria-label="Brand kit">
            <div className="section-title">
              <Sparkles size={15} />
              Brand kit
            </div>
            <p className="brand-kit-summary">{summarizeBrandKit(production.brandKit)}</p>
            <div className="brand-kit-form">
              <label className="brand-kit-field">
                <span>Kit name</span>
                <input
                  aria-label="Brand kit name"
                  onChange={(event) => updateBrandKit({ name: event.target.value })}
                  type="text"
                  value={production.brandKit.name}
                />
              </label>
              <label className="brand-kit-field">
                <span>Logo text</span>
                <input
                  aria-label="Brand logo text"
                  onChange={(event) => updateBrandKit({ logoText: event.target.value })}
                  type="text"
                  value={production.brandKit.logoText}
                />
              </label>
              <label className="brand-kit-field">
                <span>Brand color</span>
                <input
                  aria-label="Brand color"
                  onChange={(event) => updateBrandKit({ brandColor: event.target.value })}
                  type="color"
                  value={production.brandKit.brandColor}
                />
              </label>
              <label className="brand-kit-field">
                <span>Accent color</span>
                <input
                  aria-label="Accent color"
                  onChange={(event) => updateBrandKit({ accentColor: event.target.value })}
                  type="color"
                  value={production.brandKit.accentColor}
                />
              </label>
              <label className="brand-kit-field">
                <span>Background</span>
                <input
                  aria-label="Brand background color"
                  onChange={(event) => updateBrandKit({ backgroundColor: event.target.value })}
                  type="color"
                  value={production.brandKit.backgroundColor}
                />
              </label>
              <label className="brand-kit-field">
                <span>Font</span>
                <select
                  aria-label="Brand font"
                  onChange={(event) => updateBrandKit({ fontFamily: event.target.value as BrandKitFont })}
                  value={production.brandKit.fontFamily}
                >
                  {brandKitFonts.map((font) => (
                    <option key={font} value={font}>{font}</option>
                  ))}
                </select>
              </label>
              <label className="brand-kit-field">
                <span>Lower-third style</span>
                <select
                  aria-label="Lower-third style"
                  onChange={(event) => updateBrandKit({ lowerThirdStyle: event.target.value as BrandKit["lowerThirdStyle"] })}
                  value={production.brandKit.lowerThirdStyle}
                >
                  {lowerThirdStyles.map((style) => (
                    <option key={style} value={style}>{style}</option>
                  ))}
                </select>
              </label>
            </div>
            <button className="ghost-button wide" onClick={applyBrandKit}>
              <Palette size={16} />
              Apply brand kit to graphics
            </button>
          </section>

          <section className="panel">
            <div className="section-title">
              <Captions size={15} />
              Lower-third &amp; captions
            </div>
            <div className="health-grid">
              <ControlReadout label="Lower-third" value={production.captionOverlay.lowerThirdPosition.replace("-", " ")} />
              <ControlReadout label="Captions" value={production.captionOverlay.captionPosition} />
            </div>
          </section>
        </div>
      </div>

      <div aria-label="Audio tab" className={`tab-content ${activeTab === "audio" ? "active" : ""}`} hidden={activeTab !== "audio"}>
        <div className="tab-panel">
          <section className="panel">
            <div className="section-title">
              <AudioLines size={15} />
              Audio mix
            </div>
            <div className="health-grid">
              <ControlReadout label="Loudness" value={`${production.audioMix.loudnessLufs} LUFS`} />
              <ControlReadout label="Limiter" value={production.audioMix.limiterActive ? "Active" : "Idle"} />
            </div>
            <div className="participant-list">
              {visibleParticipants.map((participant) => (
                <button
                  className={`participant-row ${participant.id === selectedParticipantId ? "selected" : ""}`}
                  key={participant.id}
                  onClick={() => setSelectedParticipantId(participant.id)}
                >
                  <div className="participant-main">
                    <strong>{participant.name}</strong>
                    <span>
                      {participant.role} - {participant.breakoutRoomName} - {healthLabels[participant.health]}
                    </span>
                  </div>
                  <meter
                    min={0}
                    max={100}
                    value={production.audioMix.participants.find((mix) => mix.participantId === participant.id)?.outputLevel ?? participant.audioLevel}
                  />
                </button>
              ))}
            </div>
          </section>

          {selectedParticipant && (
            <section className="panel detail-panel">
              <div className="section-title">
                <Sparkles size={15} />
                Smart handling - audio
              </div>
              <h2>{selectedParticipant.name}</h2>
              <p>{selectedParticipant.title}</p>
              <label className="role-control">
                <span>Production role</span>
                <select
                  aria-label="Production role"
                  onChange={(event) => setSelectedParticipantRole(event.target.value as ParticipantRole)}
                  value={selectedParticipant.role}
                >
                  {participantRoles.map((role) => (
                    <option key={role} value={role}>{role}</option>
                  ))}
                </select>
              </label>
              <ControlReadout
                label="Audio gain"
                value={`${(selectedAudioMix?.gainDb ?? selectedParticipant.gainDb) > 0 ? "+" : ""}${selectedAudioMix?.gainDb ?? selectedParticipant.gainDb} dB`}
              />
              <label className="gain-control">
                <span>Manual gain</span>
                <input
                  aria-label="Manual audio gain"
                  max={12}
                  min={-12}
                  onChange={(event) => setSelectedParticipantGain(Number(event.target.value))}
                  step={1}
                  type="range"
                  value={selectedAudioMix?.manualGainDb ?? 0}
                />
                <em>{selectedAudioMix?.manualGainDb ? `${selectedAudioMix.manualGainDb > 0 ? "+" : ""}${selectedAudioMix.manualGainDb} dB` : "Auto"}</em>
              </label>
              <ControlReadout label="Audio status" value={selectedAudioMix?.status ?? "balanced"} />
              <ControlReadout label="Noise suppression" value={selectedAudioMix?.noiseSuppression ? "On" : "Off"} />
              <button className="ghost-button wide" onClick={toggleSelectedParticipantMute}>
                <Mic size={16} />
                {selectedAudioMix?.muted ? "Unmute in mix" : "Mute in mix"}
              </button>
            </section>
          )}
        </div>
      </div>

      <div aria-label="Media tab" className={`tab-content ${activeTab === "media" ? "active" : ""}`} hidden={activeTab !== "media"}>
        <div className="tab-panel">
          {selectedParticipant && (
            <section className="panel detail-panel">
              <div className="section-title">
                <Video size={15} />
                Smart handling - media
              </div>
              <h2>{selectedParticipant.name}</h2>
              <p>{selectedParticipant.title}</p>
              <ControlReadout label="Smart crop" value={`${selectedParticipant.cropConfidence}% confidence`} />
              <ControlReadout label="Crop mode" value={selectedVideoEffect.cropMode} />
              <ControlReadout label="Manual zoom" value={`${selectedVideoEffect.manualZoom.toFixed(2)}x`} />
              <ControlReadout label="Chroma key" value={selectedVideoEffect.chromaKeyEnabled ? `${selectedVideoEffect.chromaKeyColor} on` : "Off"} />
              <ControlReadout label="Spill suppression" value={`${selectedVideoEffect.spillSuppression}%`} />
              <button className="ghost-button wide" onClick={toggleSelectedCropMode}>
                <Video size={16} />
                {selectedVideoEffect.cropMode === "auto" ? "Manual crop" : "Auto crop"}
              </button>
              <button className="ghost-button wide" onClick={toggleSelectedChromaKey}>
                <Sparkles size={16} />
                {selectedVideoEffect.chromaKeyEnabled ? "Disable chroma" : "Enable chroma"}
              </button>
            </section>
          )}
        </div>
      </div>

      <div aria-label="Automation tab" className={`tab-content ${activeTab === "automation" ? "active" : ""}`} hidden={activeTab !== "automation"}>
        <div className="tab-panel">
          <section className="panel">
            <div className="section-title">
              <Bot size={15} />
              Set &amp; Forget
            </div>
            <button className="ghost-button wide" onClick={toggleAutomation}>
              <Bot size={16} />
              {production.mode === "set-and-forget" ? "Automation enabled" : "Automation disabled"}
            </button>
            <p className="auto-director-status">
              Auto: {production.autoProduction.action} {production.autoProduction.confidence}% - {production.autoProduction.reason}
            </p>
          </section>

          <section className="panel">
            <div className="section-title">
              <Clapperboard size={15} />
              Transition
            </div>
            <div className="transition-picker" aria-label="Transition controls">
              {(["cut", "fade", "slide"] as const).map((style) => (
                <button
                  className={production.transition.style === style ? "selected" : ""}
                  key={style}
                  onClick={() => setTransitionStyle(style)}
                >
                  {style}
                </button>
              ))}
            </div>
            <p className="transition-status">{production.transition.statusText}</p>
          </section>

          <section className="panel template-box">
            <div className="section-title">
              <LayoutTemplate size={15} />
              Template intelligence
            </div>
            <p>{production.magicSceneStatus}</p>
            <button className="primary-action" onClick={runMagicScene} disabled={meetingState !== "in_meeting"}>
              <Sparkles size={16} />
              Magic Scene
            </button>
          </section>
        </div>
      </div>

      <footer className="bottom-bar">
        <div className="bottom-bar-cluster bottom-bar-left">
          <button className="primary-action magic-scene-button" onClick={runMagicScene} disabled={meetingState !== "in_meeting"}>
            <Sparkles size={18} />
            <span>
              <strong>Magic Scene</strong>
              <small>AI auto-direct</small>
            </span>
          </button>
          <button className={`ghost-button automation-toggle ${production.mode === "set-and-forget" ? "selected" : ""}`} onClick={toggleAutomation}>
            <Settings2 size={18} />
            <span>
              <strong>Set &amp; Forget</strong>
              <small>Automation {production.mode === "set-and-forget" ? "On" : "Off"}</small>
            </span>
            <span className={`toggle-switch ${production.mode === "set-and-forget" ? "on" : ""}`} aria-hidden="true" />
          </button>
        </div>

        <div className="bottom-bar-cluster bottom-bar-center">
          <p className="output-status-text">{production.outputSession.statusText}</p>
          <button className="transport-button take-button" onClick={() => runCommand("t")}>
            Take
            <span className="caret" aria-hidden="true" />
          </button>
          <button className={`transport-button record-button ${production.recording ? "active" : ""}`} onClick={toggleRecording}>
            {production.recording ? <span className="live-dot" /> : <CircleDot size={16} />}
            {production.recording ? "Recording" : "Record"}
            <span className="caret" aria-hidden="true" />
          </button>
          <button className={`transport-button stream-button ${production.streaming ? "active" : ""}`} onClick={toggleStreaming}>
            <Radio size={16} />
            {production.streaming ? "Streaming" : "Stream"}
            <span className="caret" aria-hidden="true" />
          </button>
        </div>

        <div className="bottom-bar-cluster bottom-bar-right">
          <div className="outputs-status">
            <span className="connection-dot" />
            OUTPUTS
          </div>
          <StatusMetric icon={<MonitorUp size={16} />} label="Program" value={`${production.output.resolution} - ${healthLabels.live}`} />
          <StatusMetric icon={<Radio size={16} />} label="Stream" value={production.streaming ? `${production.output.bitrateMbps} Mbps` : "Idle"} />
          <StatusMetric icon={<CircleDot size={16} />} label="Record" value={production.outputSession.recordingFile ?? "Not recording"} />
          <div className="mini-stats">
            <MiniStat icon={<Gauge size={14} />} label="CPU" value={cpuLoad} />
            <MiniStat icon={<Activity size={14} />} label="Memory" value={memoryLoad} />
            <MiniStat icon={<HardDrive size={14} />} label="Disk" value={diskLoad} />
            <StatusMetric icon={<Activity size={16} />} label="Drops" value={`${production.output.droppedFrames}`} />
            <StatusMetric icon={<Clapperboard size={16} />} label="Live" value={formatElapsed(production.outputSession.elapsedSeconds)} />
          </div>
          <div className="master-audio">
            <div className="master-audio-header">
              <Volume2 size={14} />
              MASTER AUDIO
              <span className="lufs-readout">{production.audioMix.loudnessLufs} LUFS</span>
              <Settings2 size={14} className="decorative-icon" aria-hidden="true" />
            </div>
            <div className="master-meter" aria-label="Master audio levels">
              <div className="meter-bar">
                <span className="meter-fill" style={{ width: `${masterLevel}%` }} />
              </div>
              <div className="meter-bar">
                <span className="meter-fill" style={{ width: `${Math.max(0, masterLevel - 4)}%` }} />
              </div>
            </div>
          </div>
        </div>
      </footer>

      <div className="visually-hidden" aria-hidden="true">
        Command: {commandStatus}
      </div>
    </div>
  );
}

function SmartHandlingPanel({
  production,
  selectedAudioMix,
  selectedParticipant,
  selectedVideoEffect,
  setSelectedParticipantGain,
  setSelectedParticipantRole,
  toggleSelectedChromaKey,
  toggleSelectedCropMode,
  toggleSelectedParticipantMute
}: {
  production: ProductionState;
  selectedAudioMix?: ProductionState["audioMix"]["participants"][number];
  selectedParticipant: Participant;
  selectedVideoEffect: ParticipantVideoEffect;
  setSelectedParticipantGain: (gainDb: number) => void;
  setSelectedParticipantRole: (role: ParticipantRole) => void;
  toggleSelectedChromaKey: () => void;
  toggleSelectedCropMode: () => void;
  toggleSelectedParticipantMute: () => void;
}) {
  return (
    <section className="panel detail-panel expanded-handling">
      <div className="section-title">
        <Sparkles size={15} />
        Smart handling
      </div>
      <h2>{selectedParticipant.name}</h2>
      <p>{selectedParticipant.title}</p>
      <label className="role-control">
        <span>Production role</span>
        <select
          aria-label="Production role"
          onChange={(event) => setSelectedParticipantRole(event.target.value as ParticipantRole)}
          value={selectedParticipant.role}
        >
          {participantRoles.map((role) => (
            <option key={role} value={role}>
              {role}
            </option>
          ))}
        </select>
      </label>
      <ControlReadout label="Smart crop" value={`${selectedParticipant.cropConfidence}% confidence`} />
      <ControlReadout label="Crop mode" value={selectedVideoEffect.cropMode} />
      <ControlReadout label="Manual zoom" value={`${selectedVideoEffect.manualZoom.toFixed(2)}x`} />
      <ControlReadout label="Chroma key" value={selectedVideoEffect.chromaKeyEnabled ? `${selectedVideoEffect.chromaKeyColor} on` : "Off"} />
      <ControlReadout label="Spill suppression" value={`${selectedVideoEffect.spillSuppression}%`} />
      <ControlReadout
        label="Audio gain"
        value={`${(selectedAudioMix?.gainDb ?? selectedParticipant.gainDb) > 0 ? "+" : ""}${selectedAudioMix?.gainDb ?? selectedParticipant.gainDb} dB`}
      />
      <label className="gain-control">
        <span>Manual gain</span>
        <input
          aria-label="Manual audio gain"
          max={12}
          min={-12}
          onChange={(event) => setSelectedParticipantGain(Number(event.target.value))}
          step={1}
          type="range"
          value={selectedAudioMix?.manualGainDb ?? 0}
        />
        <em>{selectedAudioMix?.manualGainDb ? `${selectedAudioMix.manualGainDb > 0 ? "+" : ""}${selectedAudioMix.manualGainDb} dB` : "Auto"}</em>
      </label>
      <ControlReadout label="Audio status" value={selectedAudioMix?.status ?? "balanced"} />
      <ControlReadout label="Noise suppression" value={selectedAudioMix?.noiseSuppression ? "On" : "Off"} />
      <ControlReadout label="Lower-third" value={production.captionOverlay.lowerThirdPosition.replace("-", " ")} />
      <ControlReadout label="Captions" value={production.captionOverlay.captionPosition} />
      <button className="ghost-button wide" onClick={toggleSelectedParticipantMute}>
        <Mic size={16} />
        {selectedAudioMix?.muted ? "Unmute in mix" : "Mute in mix"}
      </button>
      <button className="ghost-button wide" onClick={toggleSelectedCropMode}>
        <Video size={16} />
        {selectedVideoEffect.cropMode === "auto" ? "Manual crop" : "Auto crop"}
      </button>
      <button className="ghost-button wide" onClick={toggleSelectedChromaKey}>
        <Sparkles size={16} />
        {selectedVideoEffect.chromaKeyEnabled ? "Disable chroma" : "Enable chroma"}
      </button>
    </section>
  );
}

function sceneLayoutIcon(layout: SceneTemplate["layout"]) {
  switch (layout) {
    case "host-focus":
      return Users;
    case "two-up":
      return Video;
    case "speaker-slides":
      return MonitorUp;
    case "smart-grid":
      return LayoutTemplate;
    case "outro":
      return Clapperboard;
    default:
      return Clapperboard;
  }
}

function MiniStat({ icon, label, value }: { icon: React.ReactNode; label: string; value: number }) {
  return (
    <div className="mini-stat">
      <div className="mini-stat-label">
        {icon}
        <span>{label}</span>
        <strong>{value}%</strong>
      </div>
      <div className="mini-stat-bar">
        <span className="mini-stat-fill" style={{ width: `${Math.min(100, Math.max(0, value))}%` }} />
      </div>
    </div>
  );
}

function ProgramGraphic({ graphic }: { graphic: GraphicOverlay }) {
  return (
    <div
      className={`program-graphic graphic-${graphic.kind} position-${graphic.position}`}
      style={{ "--graphic-accent": graphic.accent } as React.CSSProperties}
    >
      <span>{graphic.text}</span>
    </div>
  );
}

function getEnabledDestinations(destinations: OutputDestination[]) {
  return destinations.filter((destination) => destination.enabled);
}

function applyParticipantRoleOverrides(participants: Participant[], roleOverrides: Record<string, ParticipantRole>) {
  return participants.map((participant) =>
    roleOverrides[participant.id] ? { ...participant, role: roleOverrides[participant.id] } : participant
  );
}

function getSceneParticipants(scene: SceneTemplate, participants: Participant[]) {
  const sortedParticipants = sortParticipantsForProduction(participants);
  const routeParticipants = getRouteDefaults(scene, participants)
    .map((route) => resolveRouteParticipant(route, sortedParticipants))
    .filter(Boolean) as Participant[];
  const assignedParticipants = dedupeParticipants(routeParticipants);
  const fallbackParticipants = sortedParticipants.filter(
    (participant) => !assignedParticipants.some((assigned) => assigned.id === participant.id)
  );

  return [...assignedParticipants, ...fallbackParticipants].slice(0, getSceneSlotCount(scene, participants));
}

function getRouteDefaults(scene: SceneTemplate, participants: Participant[]): SourceRoute[] {
  const slotCount = getRouteSlotCount(scene, participants);
  const slotDefaults =
    scene.layout === "speaker-slides"
      ? [...getSlotDefaults(scene, participants), "screen-share"]
      : getSlotDefaults(scene, participants);
  const existingRoutes = scene.routes ?? [];

  return Array.from({ length: slotCount }, (_, index) =>
    normalizeRouteUpdate(existingRoutes[index] ?? routeFromSlot(scene.id, index, slotDefaults[index]), participants)
  );
}

function routeFromSlot(sceneId: string, index: number, slot?: string): SourceRoute {
  if (slot === "screen-share") {
    return { id: `${sceneId}-${index + 1}`, mode: "screen-share", audioRole: "audience" };
  }

  return {
    id: `${sceneId}-${index + 1}`,
    mode: slot ? "fixed" : "active-speaker",
    participantId: slot,
    audioRole: slot ? "isolated" : "mix"
  };
}

function normalizeRouteUpdate(route: SourceRoute, participants: Participant[]): SourceRoute {
  if (route.mode === "fixed") {
    return {
      ...route,
      participantId: route.participantId ?? participants[0]?.id,
      audioRole: route.audioRole === "audience" ? "isolated" : route.audioRole
    };
  }

  if (route.mode === "spotlight") {
    return {
      ...route,
      participantId: route.participantId ?? participants[route.spotlightIndex ?? 0]?.id,
      spotlightIndex: route.spotlightIndex ?? 0,
      audioRole: route.audioRole === "audience" ? "mix" : route.audioRole
    };
  }

  if (route.mode === "screen-share") {
    return { ...route, participantId: undefined, audioRole: "audience" };
  }

  if (route.mode === "none") {
    return { ...route, participantId: undefined, audioRole: "audience" };
  }

  return { ...route, participantId: undefined, audioRole: route.audioRole === "isolated" ? "mix" : route.audioRole };
}

function resolveRouteParticipant(route: SourceRoute, participants: Participant[]) {
  if (route.mode === "fixed") {
    return participants.find((participant) => participant.id === route.participantId);
  }

  if (route.mode === "active-speaker") {
    return participants.find((participant) => participant.isActiveSpeaker && participant.health !== "video-off");
  }

  if (route.mode === "spotlight") {
    return participants.find((participant) => participant.id === route.participantId) ?? participants[route.spotlightIndex ?? 0];
  }

  return undefined;
}

function routeToSlot(route: SourceRoute) {
  if (route.mode === "fixed" || route.mode === "spotlight") {
    return route.participantId;
  }

  if (route.mode === "screen-share") {
    return "screen-share";
  }

  return undefined;
}

function dedupeParticipants(participants: Participant[]) {
  const seen = new Set<string>();
  return participants.filter((participant) => {
    if (seen.has(participant.id)) {
      return false;
    }
    seen.add(participant.id);
    return true;
  });
}

function getRouteWarnings(scene: SceneTemplate, participants: Participant[]) {
  const routes = getRouteDefaults(scene, participants);
  const warnings: string[] = [];
  const fixedParticipantIds = routes
    .filter((route) => (route.mode === "fixed" || route.mode === "spotlight") && route.participantId)
    .map((route) => route.participantId as string);
  const isolatedParticipantIds = routes
    .filter((route) => route.audioRole === "isolated" && route.participantId)
    .map((route) => route.participantId as string);

  getDuplicateIds(fixedParticipantIds).forEach((participantId) => {
    const participant = participants.find((item) => item.id === participantId);
    warnings.push(`${participant?.name ?? participantId} is assigned to multiple fixed routes.`);
  });

  getDuplicateIds(isolatedParticipantIds).forEach((participantId) => {
    const participant = participants.find((item) => item.id === participantId);
    warnings.push(`${participant?.name ?? participantId} has duplicated isolated audio.`);
  });

  routes.forEach((route, index) => {
    const participant = route.participantId ? participants.find((item) => item.id === route.participantId) : undefined;

    if (route.mode === "fixed" && !participant) {
      warnings.push(`Slot ${index + 1} fixed participant is unavailable.`);
    }

    if (route.mode === "active-speaker" && !participants.some((item) => item.isActiveSpeaker && item.health !== "video-off")) {
      warnings.push(`Slot ${index + 1} active speaker is unavailable.`);
    }

    if (route.mode === "screen-share" && !participants.some((item) => item.isScreenSharing)) {
      warnings.push(`Slot ${index + 1} screen share is unavailable.`);
    }

    if (route.mode === "spotlight" && !participant) {
      warnings.push(`Slot ${index + 1} spotlight source is unavailable.`);
    }

    if (route.mode === "none") {
      warnings.push(`Slot ${index + 1} is parked.`);
    }

    if (participant?.health === "low-resolution") {
      warnings.push(`${participant.name} is below target resolution.`);
    }

    if (participant?.health === "recovering") {
      warnings.push(`${participant.name} feed is recovering.`);
    }

    if (participant?.health === "video-off") {
      warnings.push(`${participant.name} video is off.`);
    }
  });

  return [...new Set(warnings)];
}

function getDuplicateIds(ids: string[]) {
  const seen = new Set<string>();
  const duplicates = new Set<string>();

  ids.forEach((id) => {
    if (seen.has(id)) {
      duplicates.add(id);
    }
    seen.add(id);
  });

  return [...duplicates];
}

function getSlotDefaults(scene: SceneTemplate, participants: Participant[]) {
  const existingSlots = (scene.slots ?? []).filter((slot) => slot !== "screen-share");
  const sortedParticipants = sortParticipantsForProduction(participants);
  const slotCount = getSceneSlotCount(scene, participants);
  const defaults = [...existingSlots];

  sortedParticipants.forEach((participant) => {
    if (defaults.length < slotCount && !defaults.includes(participant.id)) {
      defaults.push(participant.id);
    }
  });

  return defaults.slice(0, slotCount);
}

function getRouteSlotCount(scene: SceneTemplate, participants: Participant[]) {
  if (scene.layout === "speaker-slides") {
    return 2;
  }

  return getSceneSlotCount(scene, participants);
}

function getSceneSlotCount(scene: SceneTemplate, participants: Participant[]) {
  if (scene.layout === "two-up") {
    return Math.min(2, participants.length);
  }

  if (scene.layout === "smart-grid") {
    return Math.min(6, participants.length);
  }

  return Math.min(1, participants.length);
}

function applyAutoProductionRecommendation(
  production: ProductionState,
  recommendation: AutoProductionState
): Partial<ProductionState> {
  if (recommendation.action === "hold") {
    return {
      transition: {
        ...production.transition,
        statusText: `Set & Forget holding: ${recommendation.reason}`
      }
    };
  }

  const scene = production.scenes.find((item) => item.id === recommendation.recommendedSceneId);

  return {
    ...(recommendation.action === "take" ? { activeSceneId: recommendation.recommendedSceneId } : {}),
    previewSceneId: recommendation.recommendedSceneId,
    transition: {
      ...production.transition,
      statusText: `Set & Forget ${recommendation.action === "take" ? "took" : "queued"} ${scene?.name ?? recommendation.recommendedSceneId}`
    },
    scenes: production.scenes.map((item) => ({ ...item, selected: item.id === recommendation.recommendedSceneId }))
  };
}

function ScenePreview({
  activeShareFrame,
  frames,
  participants,
  scene,
  videoEffects
}: {
  activeShareFrame: MediaFrameState | undefined;
  frames: MediaFrameState[];
  participants: Participant[];
  scene: SceneTemplate;
  videoEffects: ParticipantVideoEffect[];
}) {
  if (scene.layout === "host-focus" || scene.layout === "outro") {
    const host = participants.find((participant) => participant.role === "Host") ?? participants[0];
    const effect = host ? getVideoEffect(videoEffects, host.id) : undefined;
    const frame = host && effect ? applyVideoEffectToFrame(getFrameForParticipant(frames, host.id), effect) : undefined;

    return (
      <div className="focus-layout" aria-label={`${scene.name} layout`}>
        <div className="scene-kicker">{scene.layout === "outro" ? "Closing scene" : "Intro scene"}</div>
        {host && effect && <ParticipantTile effect={effect} frame={frame} index={0} participant={host} variant="hero" />}
        <strong>{scene.name}</strong>
        <span>{scene.automation}</span>
      </div>
    );
  }

  if (scene.layout === "two-up") {
    return (
      <div className="two-up-layout" aria-label="Interview layout">
        {participants.slice(0, 2).map((participant, index) => (
          <ParticipantTile
            effect={getVideoEffect(videoEffects, participant.id)}
            frame={applyVideoEffectToFrame(getFrameForParticipant(frames, participant.id), getVideoEffect(videoEffects, participant.id))}
            index={index}
            key={participant.id}
            participant={participant}
            variant="large"
          />
        ))}
      </div>
    );
  }

  if (scene.layout === "smart-grid") {
    return (
      <div className="grid-layout" aria-label="Smart panel grid">
        {participants.map((participant, index) => (
          <ParticipantTile
            effect={getVideoEffect(videoEffects, participant.id)}
            frame={applyVideoEffectToFrame(getFrameForParticipant(frames, participant.id), getVideoEffect(videoEffects, participant.id))}
            index={index}
            key={participant.id}
            participant={participant}
            variant="grid"
          />
        ))}
      </div>
    );
  }

  return (
    <>
      <div className="slide-share" aria-label="Speaker slides layout">
        <span>{activeShareFrame ? "Product roadmap" : "No screen share"}</span>
        <div className="slide-chart">
          <i />
          <i />
          <i />
        </div>
        <p>
          {activeShareFrame
            ? `${activeShareFrame.label} - ${activeShareFrame.width}x${activeShareFrame.height} - ${activeShareFrame.ageMs}ms`
            : "Magic Scene will reserve this region until a presenter starts sharing."}
        </p>
      </div>
      <div className="speaker-stack">
        {participants.map((participant, index) => (
          <ParticipantTile
            effect={getVideoEffect(videoEffects, participant.id)}
            frame={applyVideoEffectToFrame(getFrameForParticipant(frames, participant.id), getVideoEffect(videoEffects, participant.id))}
            index={index}
            key={participant.id}
            participant={participant}
          />
        ))}
      </div>
    </>
  );
}

function StatusMetric({ icon, label, value }: { icon: React.ReactNode; label: string; value: string }) {
  return (
    <div className="status-metric">
      {icon}
      <div>
        <span>{label}</span>
        <strong>{value}</strong>
      </div>
    </div>
  );
}

function ParticipantTile({
  effect,
  frame,
  index,
  participant,
  variant = "stack"
}: {
  effect: ParticipantVideoEffect;
  frame: MediaFrameState | undefined;
  index: number;
  participant: Participant;
  variant?: "stack" | "large" | "grid" | "hero";
}) {
  const initials = participant.name.split(" ").map((part) => part[0]).join("");
  const frameLabel = frame ? `${frame.width}x${frame.height} ${frame.fps}fps` : "Waiting for frame";
  const cropLabel = frame
    ? `crop ${Math.round(frame.crop.width * 100)}% / ${Math.round(frame.ageMs)}ms`
    : "no crop";

  return (
    <div
      className={`video-tile tile-${index + 1} tile-${variant} health-${frame?.health ?? "recovering"}`}
      style={frame ? { "--tile-accent": frame.dominantColor } as React.CSSProperties : undefined}
    >
      <div className="avatar">{initials}</div>
      <span>{participant.name}</span>
      <small>{frameLabel}</small>
      <em>{cropLabel}</em>
      <em>{effect.cropMode === "manual" ? `manual ${effect.manualZoom.toFixed(2)}x` : "auto crop"}</em>
      {effect.chromaKeyEnabled && <b>Chroma key</b>}
      {participant.isActiveSpeaker && <strong>Active speaker</strong>}
    </div>
  );
}

function ControlReadout({ label, value }: { label: string; value: string }) {
  return (
    <div className="control-readout">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

function formatElapsed(seconds: number) {
  const minutes = Math.floor(seconds / 60).toString().padStart(2, "0");
  const remainingSeconds = (seconds % 60).toString().padStart(2, "0");
  return `${minutes}:${remainingSeconds}`;
}

function isEditableTarget(target: EventTarget | null) {
  if (!(target instanceof HTMLElement)) {
    return false;
  }

  return Boolean(target.closest("input, textarea, select, [contenteditable='true']"));
}
