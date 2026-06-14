import {
  Activity,
  AudioLines,
  Bot,
  Cable,
  Captions,
  CircleDot,
  Clapperboard,
  Expand,
  FileText,
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
import { computeAutoCrop, describeFraming } from "./engine/autoCrop";
import { applyBrandKitToGraphics, programBackgroundStyle, summarizeBrandKit } from "./engine/brandKit";
import { captionStyleVars, formatCaptionText, summarizeCaptionStyle } from "./engine/captionStyle";
import { appendCaptionEntry, attributeCaption } from "./engine/captionTranscript";
import { summarizeCaptureFleet } from "./engine/captureFleet";
import { colorGradeFilter, colorGradeLuts, summarizeColorGrade } from "./engine/colorGrade";
import { controlButtonStatus, controlSurface, summarizeControlSurface, type ControlAction } from "./engine/controlSurface";
import { addMediaAsset, groupMediaBin, mediaAssetKinds, removeMediaAsset, summarizeMediaBin } from "./engine/mediaBin";
import { planMultitrackAudio } from "./engine/multitrackAudio";
import { describeTransition, recommendedTransitionDuration, transitionStyles, transitionUsesWipe } from "./engine/transitions";
import { describeVirtualCamera } from "./engine/virtualCamera";
import { getFrameForParticipant } from "./engine/mediaFrames";
import { summarizeArming } from "./engine/outputArming";
import { computeOutputProfileReadout, isUltraHdProfile } from "./engine/outputProfile";
import { runOutputPreflight, runRecordingPreflight } from "./engine/outputPreflight";
import { applyShowPreset as applyShowPresetState } from "./engine/presets";
import { addDestinationFromPreset, streamingPresets } from "./engine/streamingPresets";
import { aggregateStreamHealth, formatBitrate, scoreStreamHealth } from "./engine/streamHealth";
import { describeSrtConnection, parseSrtUrl, recommendSrtLatency } from "./engine/srtOutput";
import { describeWebRtcMonitor, parseWebRtcEndpoint } from "./engine/webrtcOutput";
import { evaluateFeedHealth, feedHealthBadgeColor, summarizeRosterHealth, type FeedHealthSignal } from "./engine/feedHealth";
import { diskSpaceSummary, evaluateDiskSpace } from "./engine/diskSpace";
import { describeNdiSource, estimateNdiBandwidth, parseNdiSourceName } from "./engine/ndiOutput";
import { buildRundownFromScenes, computeShowClock, formatClock } from "./engine/showClock";
import { formatDbtp, formatLufs, loudnessTargets, planLoudnessNormalisation } from "./engine/audioLoudness";
import { isoOutputPath, planIsoRecording, summarizeIsoPlan, validateIsoAgainstDisk } from "./engine/isoRecording";
import { decideAutoSwitch, recommendScene, summarizeSceneIntelligence, type SceneLayout } from "./engine/sceneIntelligence";
import { computeCaptionQuality, decideCaptionVisibility, detectDeadAir, summarizeCaptionQuality } from "./engine/captionQuality";
import { explainSpotlight, selectSpotlight, spotlightSummary, type SpotlightCandidate } from "./engine/participantSpotlight";
import { computeLatencyBudget, formatLatencyMs, latencyClassLabel, type ProtocolLatencyProfile } from "./engine/latencyBudget";
import { describeStinger, getStingerPreset, stingerPresets } from "./engine/stingerEngine";
import { formatKbps, planBandwidth, suggestBitrateReduction } from "./engine/bandwidthPlanner";
import { buildPreShowCues, computeCountdown, formatTMinus, markShowLive, markShowOver } from "./engine/preShowCountdown";
import { computeTally, tallyColorHex, filterByTally } from "./engine/tallyLight";
import { addMarker, formatTimecode, rippleTrim, summarizeTrim, trimClip, type ClipMarker, type ClipRegion } from "./engine/clipTrimmer";
import { addChapter, activeChapterAt, exportChapters, removeChapter, summarizeChapters, validateChapters, type ChapterList } from "./engine/chapterMarker";
import { buildSpeakerReport, createSpeakerTimer, formatSec, setActiveSpeaker, snapshotTotals, speakerSharePct } from "./engine/speakerTimer";
import { buildWatermarkSpec, classificationColor, confidentialityLevels, createWatermark, updateWatermark, validateWatermark, watermarkPositions, type WatermarkConfig } from "./engine/outputWatermark";
import { applyVideoEffectToFrame, getVideoEffect, toggleChromaKey, toggleCropMode } from "./engine/videoEffects";
import {
  getBreakoutRooms,
  initialProduction,
  sortParticipantsForProduction,
  type AiStudioState,
  type AutoProductionState,
  type BrandKit,
  type BrandKitFont,
  type CaptionFontSize,
  type CaptionStyle,
  type GraphicOverlay,
  type MediaAssetKind,
  type MediaFrameState,
  type OutputDestination,
  type OutputHealth,
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
import type { NativeMediaCoreEvent, NativeMediaCoreOperatorAction, NativeMediaCoreStateSnapshot } from "./engine/nativeMediaCoreProtocol";
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
type DiagnosticsFilter = "all" | "critical" | "warning" | "recovery";

const participantRoles: ParticipantRole[] = ["Host", "Presenter", "Panelist", "Guest"];
const exclusiveParticipantRoles = new Set<ParticipantRole>(["Host", "Presenter"]);
const diagnosticsFilters: Array<{ id: DiagnosticsFilter; label: string }> = [
  { id: "all", label: "All" },
  { id: "critical", label: "Critical" },
  { id: "warning", label: "Warnings" },
  { id: "recovery", label: "Recovery" }
];
const brandKitFonts: BrandKitFont[] = ["Inter", "Poppins", "Roboto", "Georgia"];
const lowerThirdStyles: BrandKit["lowerThirdStyle"][] = ["solid", "minimal", "gradient"];
const captionFontSizes: CaptionFontSize[] = ["small", "medium", "large"];

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
  const [aiStudio, setAiStudio] = useState<AiStudioState>();
  const [mediaCoreSnapshot, setMediaCoreSnapshot] = useState<NativeMediaCoreStateSnapshot>();
  const [showPreviewMonitor, setShowPreviewMonitor] = useState(false);
  const [commandStatus, setCommandStatus] = useState("Ready");
  const [participantRoleOverrides, setParticipantRoleOverrides] = useState<Record<string, ParticipantRole>>({});
  const [selectedParticipantId, setSelectedParticipantId] = useState("p2");
  const [activeTab, setActiveTab] = useState<TabId>("studio");
  const [diagnosticsFilter, setDiagnosticsFilter] = useState<DiagnosticsFilter>("all");
  const [selectedStreamingPresetId, setSelectedStreamingPresetId] = useState(streamingPresets[0].id);
  const [mediaAssetKind, setMediaAssetKind] = useState<MediaAssetKind>(mediaAssetKinds[0]);
  const [safeAreasEnabled, setSafeAreasEnabled] = useState(false);
  const [expandedParticipantId, setExpandedParticipantId] = useState<string | null>(null);
  const [showClockSegmentIndex, setShowClockSegmentIndex] = useState(-1);
  const [showClockSegmentElapsed, setShowClockSegmentElapsed] = useState(0);
  const [showClockTotalElapsed, setShowClockTotalElapsed] = useState(0);
  const [preShowSecondsToLive, setPreShowSecondsToLive] = useState(900);
  const [clipRegion, setClipRegion] = useState<ClipRegion>({ inPointSec: 0, outPointSec: 30, fps: 30 });
  const [clipMarkers, setClipMarkers] = useState<ClipMarker[]>([]);
  const clipSourceDurationSec = 60;
  const [chapterList, setChapterList] = useState<ChapterList>({ markers: [], recordingDurationSec: 3600 });
  const [chapterExportFormat, setChapterExportFormat] = useState<"youtube" | "vtt" | "text" | "json">("youtube");
  const [watermarkConfig, setWatermarkConfig] = useState<WatermarkConfig>(createWatermark());
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
  const selectedAutoCrop = useMemo(
    () => (selectedParticipant ? computeAutoCrop(selectedParticipant) : undefined),
    [selectedParticipant]
  );
  const captureFleet = useMemo(() => summarizeCaptureFleet(production.captureDevices), [production.captureDevices]);
  const multitrackPlan = useMemo(
    () => planMultitrackAudio(production.participants, production.recordingSettings.isoParticipantIds),
    [production.participants, production.recordingSettings.isoParticipantIds]
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
    // The live destination list is the source of truth for the armed/endpoint
    // fields the operator edits; the session contributes runtime fields only.
    return {
      ...destination,
      active: sessionState?.active ?? false,
      health: sessionState?.health ?? ("idle" as const),
      bitrateMbps: sessionState?.bitrateMbps ?? 0
    };
  });
  const mediaCoreEvents = useMemo(() => filterMediaCoreEvents(mediaCoreSnapshot?.eventLog ?? [], diagnosticsFilter), [diagnosticsFilter, mediaCoreSnapshot?.eventLog]);

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

  useEffect(() => {
    let mounted = true;

    engines.aiStudio.generate(production).then((studio) => {
      if (mounted) {
        setAiStudio(studio);
      }
    });

    return () => {
      mounted = false;
    };
  }, [
    engines.aiStudio,
    production.activeSceneId,
    production.captionOverlay,
    production.captions,
    production.meetingTitle,
    production.outputSession.elapsedSeconds,
    production.outputSession.statusText,
    production.participants,
    production.previewSceneId,
    production.recording,
    production.scenes
  ]);

  useEffect(() => {
    let mounted = true;

    engines.mediaCore.syncProduction(production, elapsedSeconds * 1000).then((snapshot) => {
      if (mounted) {
        setMediaCoreSnapshot(snapshot);
      }
    });

    return () => {
      mounted = false;
    };
  }, [
    elapsedSeconds,
    engines.mediaCore,
    production.activeSceneId,
    production.graphics,
    production.outputDestinations,
    production.participants,
    production.recording,
    production.recordingSettings.isoParticipantIds,
    production.scenes,
    production.streaming,
    production.videoEffects
  ]);

  async function selectCaptureDeviceInput(deviceId: string, inputId: string) {
    const captureDevices = await engines.captureDevices.selectInput(deviceId, inputId);
    setProduction((current) => ({ ...current, captureDevices }));
  }

  async function setCaptureDeviceAudioSyncOffset(deviceId: string, offsetMs: number) {
    const captureDevices = await engines.captureDevices.setAudioSyncOffset(deviceId, offsetMs);
    setProduction((current) => ({ ...current, captureDevices }));
  }

  async function connectCaptureDevice(deviceId: string) {
    const captureDevices = await engines.captureDevices.connectDevice(deviceId);
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
      captionTranscript: appendCaptionEntry(
        current.captionTranscript,
        attributeCaption(captionOverlay, participants, snapshot.elapsedSeconds)
      ),
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
    setProduction((current) => ({
      ...current,
      transition: {
        ...current.transition,
        style,
        durationMs: recommendedTransitionDuration(style),
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

  function triggerControlAction(action: ControlAction) {
    switch (action) {
      case "take":
        void runCommand("t");
        break;
      case "preview":
        void runCommand("p");
        break;
      case "magic-scene":
        void runMagicScene();
        break;
      case "set-and-forget":
        toggleAutomation();
        break;
      case "record":
        void toggleRecording();
        break;
      case "stream":
        void toggleStreaming();
        break;
    }
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

  function addStreamingDestination() {
    if (production.streaming) {
      return;
    }

    setProduction((current) => ({
      ...current,
      outputDestinations: addDestinationFromPreset(selectedStreamingPresetId, current.outputDestinations)
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

  function updateCaptionStyle(update: Partial<ProductionState["captionStyle"]>) {
    setProduction((current) => ({
      ...current,
      captionStyle: { ...current.captionStyle, ...update }
    }));
  }

  function updateColorGrade(update: Partial<ProductionState["colorGrade"]>) {
    setProduction((current) => ({
      ...current,
      colorGrade: { ...current.colorGrade, ...update }
    }));
  }

  function updateVirtualCamera(update: Partial<ProductionState["virtualCamera"]>) {
    setProduction((current) => ({
      ...current,
      virtualCamera: { ...current.virtualCamera, ...update }
    }));
  }

  function addBinAsset() {
    setProduction((current) => ({
      ...current,
      mediaBin: addMediaAsset(current.mediaBin, mediaAssetKind)
    }));
  }

  function removeBinAsset(assetId: string) {
    setProduction((current) => ({
      ...current,
      mediaBin: removeMediaAsset(current.mediaBin, assetId)
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
    const bundle = await engines.diagnostics.createSupportBundle(production, mediaCoreSnapshot);
    setSupportBundle(bundle);
    setSupportBundleStatus(`${bundle.id} exported`);
  }

  async function executeMediaCoreAction(action: NativeMediaCoreOperatorAction) {
    const snapshot = await engines.mediaCore.executeOperatorAction(production, action, elapsedSeconds * 1000);
    setMediaCoreSnapshot(snapshot);
    setSupportBundleStatus(`${action.title} executed`);
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
  const programResLabel = shortResolutionLabel(
    outputProfileBadge?.resolution ?? production.output.resolution,
    outputProfileBadge?.fps ?? production.output.fps
  );
  const outputProfileReadout = outputProfileBadge
    ? computeOutputProfileReadout(outputProfileBadge, production.outputDestinations)
    : undefined;
  const armingSummary = summarizeArming(production.outputDestinations);
  const armingReadinessById = new Map(armingSummary.destinations.map((destination) => [destination.id, destination]));
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
              <span className="badge">{programResLabel}</span>
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
              <div
                className={`program-canvas layout-${activeScene.layout} ${safeAreasEnabled ? "safe-areas" : ""}`}
                style={{ background: programBackgroundStyle(production.brandKit), filter: colorGradeFilter(production.colorGrade) }}
              >
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
                {transitionUsesWipe(production.transition.style) && production.transition.lastTakenSceneId && (
                  <div
                    aria-hidden="true"
                    className={`transition-wipe wipe-${production.transition.style}`}
                    key={`${production.transition.style}-${production.transition.lastTakenSceneId}`}
                    style={{
                      animationDuration: `${production.transition.durationMs}ms`,
                      "--wipe-accent": production.brandKit.brandColor
                    } as React.CSSProperties}
                  />
                )}
                <div className={`lower-third lower-third-bar position-${production.captionOverlay.lowerThirdPosition}`}>
                  <strong>{selectedParticipant?.name ?? "CoreVideo Pro"}</strong>
                  <span className="lower-third-title">{selectedParticipant?.title ?? "Zoom-native production"}</span>
                  <span className="lower-third-org">{production.brandKit.logoText.split(" ")[0]}</span>
                </div>
                {production.captionOverlay.warnings.length > 0 && (
                  <div className="overlay-warning">{production.captionOverlay.warnings[0]}</div>
                )}
                {production.graphics.filter((graphic) => graphic.enabled).map((graphic) => (
                  <ProgramGraphic key={graphic.id} graphic={graphic} />
                ))}
              </div>
              <div
                className={`caption-strip-row caption-size-${production.captionStyle.fontSize}`}
                style={captionStyleVars(production.captionStyle) as React.CSSProperties}
              >
                <span className="cc-badge">
                  <Captions size={14} />
                  CC
                </span>
                <div className="caption-strip-text">
                  <strong>{production.captionOverlay.speakerName}</strong>
                  <span>{formatCaptionText(production.captionOverlay.text, production.captionStyle)}</span>
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
              {(() => {
                const isoParticipants = production.participants
                  .filter((p) => production.recordingSettings.isoParticipantIds.includes(p.id))
                  .map((p) => ({ id: p.id, name: p.name }));
                const isoPlan = planIsoRecording(isoParticipants, {
                  includeProgramMix: true,
                  sessionName: production.recordingSettings.filenamePrefix || "session",
                });
                const diskErrors = validateIsoAgainstDisk(
                  isoPlan,
                  production.output.availableDiskGb,
                  60
                );
                return (
                  <div className="iso-plan" aria-label="ISO recording plan">
                    <span className="iso-plan-summary">{summarizeIsoPlan(isoPlan)}</span>
                    {isoPlan.valid && (
                      <div className="iso-track-list">
                        {isoPlan.tracks.map((track) => (
                          <span className={`iso-track iso-track-${track.source}`} key={track.trackIndex}>
                            {isoOutputPath(isoPlan.outputFolder, track.label, track.trackIndex)
                              .split("/")
                              .pop()}
                          </span>
                        ))}
                      </div>
                    )}
                    {[...isoPlan.warnings, ...diskErrors].map((w) => (
                      <p className="multitrack-warning" role="status" key={w}>{w}</p>
                    ))}
                  </div>
                );
              })()}
              <div className="multitrack-plan" aria-label="Multitrack audio plan">
                <span className="multitrack-summary">{multitrackPlan.summary}</span>
                <div className="multitrack-tracks">
                  {multitrackPlan.tracks.map((track) => (
                    <span className={`multitrack-track track-${track.kind} ${track.muted ? "muted" : ""}`} key={track.id}>
                      {track.label} - {track.channels === 2 ? "stereo" : "mono"}
                    </span>
                  ))}
                </div>
                {multitrackPlan.warnings.map((warning) => (
                  <p className="multitrack-warning" role="status" key={warning}>{warning}</p>
                ))}
              </div>
              {(() => {
                const videoRateMbps = outputProfileBadge?.targetBitrateMbps ?? production.output.bitrateMbps;
                const diskRateMBps = multitrackPlan.estimatedDiskRateMBps + videoRateMbps / 8;
                const disk = evaluateDiskSpace(production.output.availableDiskGb, diskRateMBps);
                return (
                  <div className={`disk-space-status level-${disk.level}`} aria-label="Disk space status">
                    <HardDrive size={13} />
                    <span className="disk-space-summary">{diskSpaceSummary(disk)}</span>
                    {disk.warnings.map((w) => (
                      <span key={w} className="disk-space-warning">{w}</span>
                    ))}
                  </div>
                );
              })()}
            </div>
          </section>

          <section className="panel">
            <div className="section-title">
              <Radio size={15} />
              Destinations
            </div>
            <div className="arming-summary" aria-label="Arming readiness">
              <strong>{armingSummary.summary}</strong>
              {armingSummary.blockers.length > 0 ? (
                <ul className="arming-blockers">
                  {armingSummary.blockers.map((blocker) => (
                    <li key={blocker}>{blocker}</li>
                  ))}
                </ul>
              ) : (
                <span>{armingSummary.armedCount > 0 ? "All armed destinations are ready to go live." : "Arm a destination to go live."}</span>
              )}
            </div>
            <div className="destination-list">
              {destinationStates.map((destination) => {
                const readiness = armingReadinessById.get(destination.id);
                return (
                <div
                  className={`destination-row ${destination.enabled ? "enabled" : ""} ${destination.active ? "active" : ""} ${
                    destination.enabled && readiness && !readiness.ready ? "needs-attention" : ""
                  }`}
                  key={destination.id}
                >
                  <button disabled={production.streaming} onClick={() => toggleOutputDestination(destination.id)}>
                    <div>
                      <strong>{destination.name}</strong>
                      <span>
                        {destination.protocol} - {destination.latencyMs} ms
                      </span>
                    </div>
                    <em>
                      {destination.active
                        ? `${destination.health} ${destination.bitrateMbps} Mbps`
                        : destination.enabled
                          ? readiness?.statusLabel ?? "Armed"
                          : "Off"}
                    </em>
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
                  {destination.enabled && readiness && (
                    <p className={`destination-readiness ${readiness.ready ? "ready" : "needs-attention"}`}>{readiness.detail}</p>
                  )}
                  {destination.protocol === "SRT" && destination.endpoint && (() => {
                    const srt = parseSrtUrl(destination.endpoint);
                    if (!srt.valid) return null;
                    const params = srt.params!;
                    const recommended = recommendSrtLatency(destination.latencyMs);
                    return (
                      <div className="srt-detail" aria-label={`${destination.name} SRT detail`}>
                        <span>{describeSrtConnection(params)}</span>
                        {recommended !== params.latencyMs && (
                          <span className="srt-latency-hint">Recommended latency: {recommended} ms (2.5× RTT)</span>
                        )}
                        {srt.warnings.map((w) => (
                          <span key={w} className="srt-warning">{w}</span>
                        ))}
                      </div>
                    );
                  })()}
                  {destination.protocol === "WebRTC" && destination.endpoint && (() => {
                    const webrtc = parseWebRtcEndpoint(destination.endpoint, destination.streamKey ?? "");
                    if (!webrtc.valid) return null;
                    const monitor = describeWebRtcMonitor(webrtc.params!);
                    return (
                      <div className="srt-detail" aria-label={`${destination.name} WebRTC detail`}>
                        <span>{monitor.summary}</span>
                        <span className={`webrtc-latency-class latency-${monitor.latencyClass}`}>
                          {monitor.latencyLabel} · up to {monitor.maxViewers} viewers
                        </span>
                        {webrtc.warnings.map((w) => (
                          <span key={w} className="srt-warning">{w}</span>
                        ))}
                      </div>
                    );
                  })()}
                  {destination.protocol === "NDI" && destination.endpoint && (() => {
                    const ndi = parseNdiSourceName(destination.endpoint);
                    const bandwidth = estimateNdiBandwidth(
                      outputProfileBadge?.resolution ?? production.output.resolution,
                      outputProfileBadge?.fps ?? production.output.fps
                    );
                    return (
                      <div className="srt-detail" aria-label={`${destination.name} NDI detail`}>
                        {ndi.valid && ndi.sourceName && (
                          <span>{describeNdiSource(ndi.sourceName)}</span>
                        )}
                        <span className={bandwidth.bandwidthWarning ? "srt-latency-hint" : ""}>
                          {bandwidth.summary}
                        </span>
                        {ndi.warnings.map((w) => (
                          <span key={w} className="srt-warning">{w}</span>
                        ))}
                      </div>
                    );
                  })()}
                </div>
                );
              })}
            </div>
            <div className="add-destination" aria-label="Add streaming destination">
              <label>
                <span>Streaming preset</span>
                <select
                  aria-label="Streaming preset"
                  disabled={production.streaming}
                  onChange={(event) => setSelectedStreamingPresetId(event.target.value)}
                  value={selectedStreamingPresetId}
                >
                  {streamingPresets.map((preset) => (
                    <option key={preset.id} value={preset.id}>{preset.name}</option>
                  ))}
                </select>
              </label>
              <button className="ghost-button wide" disabled={production.streaming} onClick={addStreamingDestination}>
                <Plus size={16} />
                Add destination
              </button>
            </div>
          </section>

          <section className="panel" aria-label="Bandwidth plan">
            <div className="section-title">
              <Wifi size={15} />
              Bandwidth plan
            </div>
            {(() => {
              const activeDests = destinationStates
                .filter((d) => d.enabled || d.active)
                .map((d) => ({
                  id: d.id,
                  name: d.name,
                  protocol: d.protocol,
                  bitrateKbps: production.output.bitrateMbps * 1000,
                }));
              const bwPlan = planBandwidth(activeDests, { uplinkCapacityMbps: 50 });
              const reduction = suggestBitrateReduction(bwPlan);
              return (
                <div className={`bandwidth-plan-panel status-${bwPlan.status}`} aria-label="Bandwidth plan panel">
                  <div className="bandwidth-plan-summary">{bwPlan.summary}</div>
                  <div className="health-grid">
                    <ControlReadout label="Total" value={formatKbps(bwPlan.totalKbps)} />
                    <ControlReadout label="Uplink" value={formatKbps(bwPlan.uplinkCapacityKbps)} />
                    <ControlReadout label="Usage" value={`${Math.round(bwPlan.usageFraction * 100)}%`} />
                    <ControlReadout label="Status" value={bwPlan.status} />
                  </div>
                  {bwPlan.destinations.map((d) => (
                    <div key={d.destinationId} className="bandwidth-dest-row">
                      <span className="bandwidth-dest-name">{d.name}</span>
                      <span className="bandwidth-dest-total">{formatKbps(d.totalKbps)}</span>
                      <span className="bandwidth-dest-proto">{d.protocol} +{Math.round((d.overheadKbps / (d.videoBitrateKbps + d.audioBitrateKbps)) * 100)}% overhead</span>
                    </div>
                  ))}
                  {reduction && (
                    <p className="multitrack-warning" role="status">
                      Reduce each destination to ~{formatKbps(reduction.targetKbpsPerDestination)} (−{reduction.reductionPct}%) to stay within budget.
                    </p>
                  )}
                  {bwPlan.warnings.filter((w) => !w.includes("Reduce")).map((w) => (
                    <p className="multitrack-warning" role="status" key={w}>{w}</p>
                  ))}
                </div>
              );
            })()}
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
                  <strong>
                    {profile.name}
                    {isUltraHdProfile(profile) && <span className="profile-tag">4K</span>}
                  </strong>
                  <span>
                    {profile.resolution} - {profile.fps}fps - {profile.targetBitrateMbps} Mbps
                  </span>
                </button>
              ))}
            </div>
            {outputProfileReadout && (
              <div className="encoder-readout" aria-label="Encoder headroom">
                <div className="health-grid">
                  <ControlReadout label="Encoder headroom" value={`${outputProfileReadout.encoderHeadroomPercent}%`} />
                  <ControlReadout label="Upload bandwidth" value={`${outputProfileReadout.uploadBandwidthMbps} Mbps`} />
                  <ControlReadout label="Stream targets" value={`${outputProfileReadout.networkDestinationCount} network`} />
                </div>
                {outputProfileReadout.warnings.length > 0 && (
                  <ul className="encoder-warnings">
                    {outputProfileReadout.warnings.map((warning) => (
                      <li key={warning}>{warning}</li>
                    ))}
                  </ul>
                )}
              </div>
            )}
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
              <ControlReadout label="Recording file" value={production.outputSession.recordingFile ?? "Not recording"} />
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

          <section className="panel" aria-label="Stream health">
            <div className="section-title">
              <Wifi size={15} />
              Stream health
            </div>
            {(() => {
              const targetKbps = (outputProfileBadge?.targetBitrateMbps ?? 6) * 1000;
              const activeDestinations = destinationStates.filter((d) => d.active);
              if (activeDestinations.length === 0) {
                return <p className="brand-kit-summary">No active destinations — health scoring available during live output.</p>;
              }
              const reports = activeDestinations.map((dest) =>
                scoreStreamHealth(
                  {
                    bitrateKbps: dest.bitrateMbps * 1000,
                    droppedFrames: production.output.droppedFrames,
                    rttMs: dest.latencyMs,
                    encoderLoadPct: production.output.encoderLoad,
                    timestampMs: Date.now(),
                  },
                  targetKbps
                )
              );
              const agg = aggregateStreamHealth(reports);
              return (
                <div className="stream-health-panel">
                  <div className={`stream-health-aggregate grade-${agg.worstGrade}`}>
                    <strong>{agg.worstGrade.charAt(0).toUpperCase() + agg.worstGrade.slice(1)}</strong>
                    <span>Avg score: {agg.avgScore}/100</span>
                  </div>
                  <div className="stream-health-destinations">
                    {activeDestinations.map((dest, i) => {
                      const report = reports[i];
                      return (
                        <div key={dest.id} className={`stream-health-dest grade-${report.grade}`}>
                          <div className="stream-health-dest-header">
                            <strong>{dest.name}</strong>
                            <span className={`health-badge grade-${report.grade}`}>{report.score}/100</span>
                          </div>
                          <div className="stream-health-metrics">
                            <span>Bitrate: {formatBitrate(report.bitrateKbps)}</span>
                            <span>RTT: {report.rttMs} ms</span>
                            <span>Encoder: {report.encoderLoadPct}%</span>
                            <span>Drops: {report.droppedFrames}</span>
                          </div>
                          {report.warnings.length > 0 && (
                            <ul className="stream-health-warnings">
                              {report.warnings.map((w) => <li key={w}>{w}</li>)}
                            </ul>
                          )}
                        </div>
                      );
                    })}
                  </div>
                </div>
              );
            })()}
          </section>

          <section className="panel" aria-label="Latency budget">
            <div className="section-title">
              <Activity size={15} />
              Latency budget
            </div>
            {(() => {
              const armedDestination = destinationStates.find((d) => d.active || d.enabled);
              const protocol = armedDestination?.protocol ?? "rtmp-hls";
              const protocolMap: Record<string, ProtocolLatencyProfile> = {
                RTMP: "rtmp-hls",
                SRT: "srt",
                WebRTC: "webrtc-whip",
                NDI: "ndi",
              };
              const latencyProtocol: ProtocolLatencyProfile = protocolMap[protocol] ?? "rtmp-hls";
              const budget = computeLatencyBudget({
                protocol: latencyProtocol,
                outputFps: production.output.fps || 30,
              });
              return (
                <div className="latency-budget-panel" aria-label="Latency budget panel">
                  <div className={`latency-class-badge class-${budget.latencyClass}`}>
                    {latencyClassLabel(budget.latencyClass)}
                  </div>
                  <p className="latency-summary">{budget.summary}</p>
                  <div className="latency-stages">
                    {budget.stages.map((stage) => (
                      <div key={stage.stage} className="latency-stage-row">
                        <span className="latency-stage-label">{stage.label}</span>
                        <span className="latency-stage-value">{formatLatencyMs(stage.estimatedMs)}</span>
                      </div>
                    ))}
                  </div>
                </div>
              );
            })()}
          </section>

          <section className="panel" aria-label="Virtual camera">
            <div className="section-title">
              <Video size={15} />
              Virtual camera
            </div>
            <p className="brand-kit-summary">{describeVirtualCamera(production.virtualCamera, production.output).label}</p>
            <label className="brand-kit-field">
              <span>Device name</span>
              <input
                aria-label="Virtual camera device name"
                onChange={(event) => updateVirtualCamera({ deviceName: event.target.value })}
                type="text"
                value={production.virtualCamera.deviceName}
              />
            </label>
            <ControlReadout label="Published format" value={describeVirtualCamera(production.virtualCamera, production.output).outputFormat} />
            <label className="caption-uppercase-field brand-kit-field">
              <span>Mirror output</span>
              <input
                aria-label="Mirror virtual camera"
                checked={production.virtualCamera.mirrored}
                onChange={(event) => updateVirtualCamera({ mirrored: event.target.checked })}
                type="checkbox"
              />
            </label>
            <button
              className={`ghost-button wide ${production.virtualCamera.enabled ? "selected" : ""}`}
              onClick={() => updateVirtualCamera({ enabled: !production.virtualCamera.enabled })}
            >
              <Video size={16} />
              {production.virtualCamera.enabled ? "Stop virtual camera" : "Start virtual camera"}
            </button>
          </section>

          <section className="panel">
            <div className="section-title">
              <Gauge size={15} />
              Native core
            </div>
            <div className="health-grid" aria-label="Native core sync">
              <ControlReadout label="Synced scene" value={mediaCoreSnapshot?.sceneId ?? "Pending"} />
              <ControlReadout label="Routes" value={`${mediaCoreSnapshot?.routeCount ?? 0}`} />
              <ControlReadout label="Frames" value={`${mediaCoreSnapshot?.frameCount ?? 0}`} />
              <ControlReadout label="Transforms" value={`${mediaCoreSnapshot?.participantTransformCount ?? 0}`} />
              <ControlReadout label="Overlays" value={`${mediaCoreSnapshot?.overlayCount ?? 0}`} />
              <ControlReadout label="Sources" value={`${mediaCoreSnapshot?.sourceCount ?? 0}`} />
              <ControlReadout label="Resolved routes" value={`${mediaCoreSnapshot?.resolvedRouteCount ?? 0}`} />
              <ControlReadout label="Render plan" value={`${mediaCoreSnapshot?.renderPlan.layers.length ?? 0} layers`} />
              <ControlReadout label="Plan ID" value={mediaCoreSnapshot?.renderPlan.renderPlanId ?? "None"} />
              <ControlReadout label="Media source" value={mediaCoreSnapshot?.sourceSnapshot.kind ?? "Idle"} />
              <ControlReadout label="Program frames" value={`${mediaCoreSnapshot?.programFrameCount ?? 0}`} />
              <ControlReadout label="Transport" value={mediaCoreSnapshot?.programTransport.status ?? "Idle"} />
              <ControlReadout label="Compositor" value={mediaCoreSnapshot?.compositor.status ?? "Idle"} />
              <ControlReadout label="Encoder" value={mediaCoreSnapshot?.encoderSession.lifecycle.status ?? "Idle"} />
              <ControlReadout label="Senders" value={mediaCoreSnapshot ? `${mediaCoreSnapshot.outputSenderSession.activeSenderCount} ${mediaCoreSnapshot.outputSenderSession.status}` : "0 idle"} />
              <ControlReadout label="Outputs" value={mediaCoreSnapshot?.outputs.length ? mediaCoreSnapshot.outputs.join(", ") : "Idle"} />
              <ControlReadout label="Profile" value={mediaCoreSnapshot?.outputProfile.profileId ?? "1080p60"} />
              <ControlReadout label="Recording" value={mediaCoreSnapshot?.recording?.status ?? "Off"} />
              <ControlReadout label="Recorded frames" value={`${mediaCoreSnapshot?.recording?.totalFramesWritten ?? 0}`} />
              <ControlReadout label="Disk rate" value={mediaCoreSnapshot?.recording ? `${mediaCoreSnapshot.recording.estimatedDiskRateMBps} MB/s` : "0 MB/s"} />
              <ControlReadout label="Output health" value={mediaCoreSnapshot?.outputHealth[0]?.status ?? "Idle"} />
              <ControlReadout label="Action" value={mediaCoreSnapshot?.operatorActions[0]?.title ?? "None"} />
              <ControlReadout label="Events" value={mediaCoreSnapshot?.eventLog.length ? `${mediaCoreSnapshot.eventLog.length} - ${mediaCoreSnapshot.eventLog.at(-1)?.title}` : "None"} />
              <ControlReadout label="Warnings" value={mediaCoreSnapshot?.warnings[0] ?? "None"} />
            </div>
          </section>

          <section className="panel media-core-diagnostics" aria-label="Media core diagnostics">
            <div className="section-title">
              <Activity size={15} />
              Diagnostics
            </div>
            <div className="health-grid">
              <ControlReadout label="Action queue" value={`${mediaCoreSnapshot?.operatorActions.length ?? 0}`} />
              <ControlReadout label="Timeline" value={`${mediaCoreSnapshot?.eventLog.length ?? 0} events`} />
            </div>
            <div className="diagnostics-filter" aria-label="Media core diagnostics filter">
              {diagnosticsFilters.map((filter) => (
                <button
                  className={diagnosticsFilter === filter.id ? "selected" : ""}
                  key={filter.id}
                  onClick={() => setDiagnosticsFilter(filter.id)}
                >
                  {filter.label}
                </button>
              ))}
            </div>
            {mediaCoreSnapshot?.operatorActions.length ? (
              <div className="operator-action-list" aria-label="Media core actions">
                {mediaCoreSnapshot.operatorActions.map((action) => (
                  <button
                    className={`operator-action operator-action-${action.severity}`}
                    disabled={!action.command}
                    key={action.actionId}
                    onClick={() => void executeMediaCoreAction(action)}
                  >
                    <RefreshCw size={15} />
                    <span>
                      <strong>{action.title}</strong>
                      <em>{action.detail}</em>
                    </span>
                  </button>
                ))}
              </div>
            ) : null}
            <div className="event-timeline" aria-label="Media core event timeline">
              {mediaCoreEvents.length ? (
                mediaCoreEvents.map((event) => (
                  <div className={`event-row event-row-${event.severity}`} key={event.eventId}>
                    <span>{formatEventTime(event.atMs)}</span>
                    <strong>{event.title}</strong>
                    <em>{event.detail}</em>
                  </div>
                ))
              ) : (
                <p className="preset-status">No matching media-core events</p>
              )}
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

          <section className="panel" aria-label="Feed health roster">
            <div className="section-title">
              <Wifi size={15} />
              Zoom feed health
            </div>
            {(() => {
              const signals: FeedHealthSignal[] = visibleParticipants.map((p) => ({
                hasVideo: p.health !== "video-off",
                hasAudio: !p.isMuted,
                isMuted: p.isMuted,
                isVideoOff: p.health === "video-off",
                resolutionTier: p.health === "low-resolution" ? 1 : p.health === "video-off" ? 0 : 2,
                msSinceLastFrame: p.health === "recovering" ? 6000 : 50,
                isReconnecting: p.health === "recovering",
              }));
              const statuses = signals.map(evaluateFeedHealth);
              const roster = summarizeRosterHealth(statuses);
              return (
                <div className="feed-health-roster">
                  <p className="feed-health-summary" aria-label="Feed health summary">{roster.summary}</p>
                  <div className="feed-health-list">
                    {visibleParticipants.map((p, i) => {
                      const status = statuses[i];
                      const color = feedHealthBadgeColor(status.state);
                      return (
                        <div key={p.id} className={`feed-health-row color-${color}`}>
                          <span className="feed-health-name">{p.name}</span>
                          <span className="feed-health-role">{p.role}</span>
                          <span className={`feed-health-badge color-${color}`}>{status.label}</span>
                          {status.needsAttention && (
                            <span className="feed-health-detail">{status.detail}</span>
                          )}
                        </div>
                      );
                    })}
                  </div>
                </div>
              );
            })()}
          </section>

          <section className="panel" aria-label="Participant spotlight">
            <div className="section-title">
              <Sparkles size={15} />
              Participant spotlight
            </div>
            {(() => {
              const roleWeights: Record<string, number> = {
                Host: 4, Presenter: 3, Guest: 2, Panelist: 1,
              };
              const candidates: SpotlightCandidate[] = visibleParticipants.map((p) => ({
                id: p.id,
                name: p.name,
                isActiveSpeaker: p.isActiveSpeaker,
                hasVideo: p.health !== "video-off",
                roleWeight: roleWeights[p.role] ?? 1,
                secondsSinceLastSpoke: p.isActiveSpeaker ? 0 : 45,
                totalSpeakingSeconds: p.audioLevel * 2,
              }));
              const decision = selectSpotlight(candidates, { pickSecondary: true });
              return (
                <div className="spotlight-panel" aria-label="Spotlight decision">
                  <div className="spotlight-summary">{spotlightSummary(decision)}</div>
                  <div className="spotlight-list">
                    {decision.scores.filter((s) => s.score > 0).map((s) => (
                      <div key={s.participantId} className={`spotlight-row ${s.participantId === decision.primaryId ? "primary" : s.participantId === decision.secondaryId ? "secondary" : ""}`}>
                        <span className="spotlight-name">{s.name}</span>
                        <span className="spotlight-score">{s.score} pts</span>
                        <span className="spotlight-reason">{explainSpotlight(s)}</span>
                      </div>
                    ))}
                  </div>
                </div>
              );
            })()}
          </section>

          <section className="panel" aria-label="Tally lights">
            <div className="section-title">
              <Radio size={15} />
              Tally lights
            </div>
            {(() => {
              const activeScene = production.scenes.find((s: SceneTemplate) => s.id === production.activeSceneId) ?? production.scenes[0];
              const previewScene = production.scenes.find((s: SceneTemplate) => s.id === production.previewSceneId) ?? activeScene;
              const programIds = (activeScene?.slots ?? []);
              const previewIds = (previewScene?.slots ?? []).filter((id) => !programIds.includes(id));
              const tallySources = visibleParticipants.map((p) => ({ sourceId: p.id, label: p.name }));
              if (screenShareActive) {
                tallySources.push({ sourceId: "screen-share", label: "Screen Share" });
              }
              const snap = computeTally(tallySources, programIds, previewIds);
              return (
                <div className="tally-panel" aria-label="Tally panel">
                  <p className="tally-summary">{snap.summary}</p>
                  <div className="tally-grid">
                    {snap.sources.map((src) => (
                      <div
                        key={src.sourceId}
                        className={`tally-row tally-${src.tally}`}
                        aria-label={`${src.label} tally`}
                      >
                        <span
                          className="tally-dot"
                          style={{ background: tallyColorHex(src.tally) }}
                        />
                        <span className="tally-label">{src.label}</span>
                        <span className="tally-state">{src.tally}</span>
                      </div>
                    ))}
                  </div>
                  <div className="tally-counts">
                    <ControlReadout label="On air" value={String(filterByTally(snap, "program").length)} />
                    <ControlReadout label="In preview" value={String(filterByTally(snap, "preview").length)} />
                    <ControlReadout label="Idle" value={String(filterByTally(snap, "idle").length)} />
                  </div>
                </div>
              );
            })()}
          </section>

          <section className="panel" aria-label="Speaker timer">
            <div className="section-title">
              <Gauge size={15} />
              Speaker timer
            </div>
            {(() => {
              const activeSpeakerId = visibleParticipants.find((p) => p.isActiveSpeaker)?.id ?? null;
              const nowSec = elapsedSeconds;
              let timerState = createSpeakerTimer(
                visibleParticipants.map((p) => ({ id: p.id, name: p.name })),
                300 // 5 min target per speaker
              );
              timerState = setActiveSpeaker(timerState, nowSec - 30, activeSpeakerId);
              timerState = snapshotTotals(timerState, nowSec);
              const report = buildSpeakerReport(timerState);
              return (
                <div className="speaker-timer-panel" aria-label="Speaker timer panel">
                  <p className="speaker-timer-summary">{report.summary}</p>
                  <div className="speaker-timer-list">
                    {report.state.speakers.map((s) => (
                      <div
                        key={s.participantId}
                        className={`speaker-row ${s.participantId === report.dominantSpeakerId ? "dominant" : ""} ${s.isSpeaking ? "speaking" : ""}`}
                        aria-label={`${s.name} speaker time`}
                      >
                        <span className="speaker-name">{s.name}</span>
                        <span className="speaker-time">{formatSec(s.totalSec)}</span>
                        <span className="speaker-share">{speakerSharePct(s, report.state.speakers)}%</span>
                        <div className="speaker-bar-wrap">
                          <div
                            className="speaker-bar"
                            style={{ width: `${speakerSharePct(s, report.state.speakers)}%` }}
                          />
                        </div>
                      </div>
                    ))}
                  </div>
                  <div className="health-grid">
                    <ControlReadout label="Balance" value={`${report.balanceScore}%`} />
                    <ControlReadout label="Alerts" value={String(report.alerts.length)} />
                  </div>
                  {report.alerts.map((alert) => (
                    <p key={alert.participantId} className={`speaker-alert speaker-alert-${alert.severity}`} role="status">
                      {alert.message}
                    </p>
                  ))}
                </div>
              );
            })()}
          </section>

          <section className="panel" aria-label="Capture devices">
            <div className="section-title">
              <Cable size={15} />
              Capture Devices
            </div>
            {production.captureDevices.length === 0 && <p className="preset-status">No Blackmagic or AJA devices detected</p>}
            {production.captureDevices.length > 0 && (
              <div className="capture-fleet" aria-label="Capture fleet">
                <p className="capture-fleet-summary">{captureFleet.summary}</p>
                {captureFleet.dualCapture && <span className="capture-fleet-badge">Dual capture live</span>}
                {captureFleet.warnings.map((warning) => (
                  <p className="capture-fleet-warning" role="status" key={warning}>{warning}</p>
                ))}
              </div>
            )}
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
                {device.connectionState !== "connected" && (
                  <button className="ghost-button wide" onClick={() => connectCaptureDevice(device.id)}>
                    <Cable size={16} />
                    Bring online as program source
                  </button>
                )}
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
              <label className="brand-kit-field brand-kit-field-wide">
                <span>Background image URL</span>
                <input
                  aria-label="Brand background image URL"
                  onChange={(event) => updateBrandKit({ backgroundImageUrl: event.target.value })}
                  placeholder="https://…  (blank for branded gradient)"
                  type="url"
                  value={production.brandKit.backgroundImageUrl}
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

          <section className="panel" aria-label="Caption style">
            <div className="section-title">
              <Captions size={15} />
              Lower-third &amp; captions
            </div>
            <div className="health-grid">
              <ControlReadout label="Lower-third" value={production.captionOverlay.lowerThirdPosition.replace("-", " ")} />
              <ControlReadout label="Captions" value={production.captionOverlay.captionPosition} />
            </div>
            <p className="brand-kit-summary">{summarizeCaptionStyle(production.captionStyle)}</p>
            <div className="brand-kit-form">
              <label className="brand-kit-field">
                <span>Caption size</span>
                <select
                  aria-label="Caption size"
                  onChange={(event) => updateCaptionStyle({ fontSize: event.target.value as CaptionFontSize })}
                  value={production.captionStyle.fontSize}
                >
                  {captionFontSizes.map((size) => (
                    <option key={size} value={size}>{size}</option>
                  ))}
                </select>
              </label>
              <label className="brand-kit-field">
                <span>Caption color</span>
                <input
                  aria-label="Caption text color"
                  onChange={(event) => updateCaptionStyle({ textColor: event.target.value })}
                  type="color"
                  value={production.captionStyle.textColor}
                />
              </label>
              <label className="brand-kit-field">
                <span>Backdrop opacity</span>
                <input
                  aria-label="Caption backdrop opacity"
                  max={100}
                  min={0}
                  onChange={(event) => updateCaptionStyle({ backgroundOpacity: Number(event.target.value) })}
                  step={5}
                  type="range"
                  value={production.captionStyle.backgroundOpacity}
                />
              </label>
              <label className="brand-kit-field caption-uppercase-field">
                <span>Uppercase</span>
                <input
                  aria-label="Uppercase captions"
                  checked={production.captionStyle.uppercase}
                  onChange={(event) => updateCaptionStyle({ uppercase: event.target.checked })}
                  type="checkbox"
                />
              </label>
            </div>
          </section>

          <section className="panel" aria-label="Speaker captions">
            <div className="section-title">
              <Captions size={15} />
              Speaker captions
            </div>
            <div className="transcript-list">
              {production.captionTranscript.map((entry) => (
                <div className="transcript-entry" key={entry.id}>
                  <div className="transcript-meta">
                    <strong>{entry.speakerName}</strong>
                    <span>{entry.role} - {entry.confidence}%</span>
                  </div>
                  <p>{entry.text}</p>
                </div>
              ))}
            </div>
          </section>

          <section className="panel" aria-label="Caption quality">
            <div className="section-title">
              <Activity size={15} />
              Caption quality
            </div>
            {(() => {
              const confidenceValues = production.captionTranscript.map((e) => e.confidence);
              const qualityReport = computeCaptionQuality(confidenceValues);
              const hasActiveSpeaker = production.participants.some((p) => p.isActiveSpeaker);
              const currentText = production.captionOverlay.text;
              const signal = {
                confidencePct: production.captionOverlay.confidence,
                msSinceLastWord: hasActiveSpeaker ? 500 : 5000,
                text: currentText,
                activeSpeakerPresent: hasActiveSpeaker,
                screenShareActive,
              };
              const visibility = decideCaptionVisibility(signal);
              const deadAir = detectDeadAir(signal.msSinceLastWord);
              return (
                <div className={`caption-quality-panel tier-${qualityReport.tier}`} aria-label="Caption quality panel">
                  <div className="caption-quality-summary">{summarizeCaptionQuality(qualityReport)}</div>
                  <div className="health-grid">
                    <ControlReadout label="Avg confidence" value={`${qualityReport.avgConfidence}%`} />
                    <ControlReadout label="Min confidence" value={`${qualityReport.minConfidence}%`} />
                    <ControlReadout label="Tier" value={qualityReport.tier} />
                    <ControlReadout label="Visibility" value={visibility.state} />
                    <ControlReadout label="Dead air" value={deadAir} />
                    <ControlReadout label="Samples" value={String(qualityReport.sampleCount)} />
                  </div>
                  {qualityReport.warnings.map((w) => (
                    <p className="multitrack-warning" role="status" key={w}>{w}</p>
                  ))}
                </div>
              );
            })()}
          </section>

          <section className="panel" aria-label="Output watermark">
            <div className="section-title">
              <Shield size={15} />
              Output watermark
            </div>
            {(() => {
              const spec = buildWatermarkSpec(watermarkConfig);
              const issues = validateWatermark(watermarkConfig);
              return (
                <div className="watermark-panel" aria-label="Watermark panel">
                  <p className="watermark-summary">{spec.summary}</p>
                  <label className="role-control">
                    <span>Mode</span>
                    <select
                      aria-label="Watermark mode"
                      value={watermarkConfig.mode}
                      onChange={(e) => setWatermarkConfig((c) => updateWatermark(c, { mode: e.target.value as WatermarkConfig["mode"] }))}
                    >
                      <option value="disabled">Disabled</option>
                      <option value="live">Live overlay</option>
                      <option value="burn-in">Burn-in</option>
                    </select>
                  </label>
                  <label className="role-control">
                    <span>Text</span>
                    <input
                      aria-label="Watermark text"
                      value={watermarkConfig.text}
                      onChange={(e) => setWatermarkConfig((c) => updateWatermark(c, { text: e.target.value }))}
                      placeholder="e.g. DRAFT, © 2025"
                    />
                  </label>
                  <label className="role-control">
                    <span>Position</span>
                    <select
                      aria-label="Watermark position"
                      value={watermarkConfig.position}
                      onChange={(e) => setWatermarkConfig((c) => updateWatermark(c, { position: e.target.value as WatermarkConfig["position"] }))}
                    >
                      {watermarkPositions.map((pos) => (
                        <option key={pos} value={pos}>{pos}</option>
                      ))}
                    </select>
                  </label>
                  <label className="gain-control">
                    <span>Opacity</span>
                    <input
                      aria-label="Watermark opacity"
                      type="range"
                      min={0}
                      max={100}
                      value={watermarkConfig.opacityPct}
                      onChange={(e) => setWatermarkConfig((c) => updateWatermark(c, { opacityPct: Number(e.target.value) }))}
                    />
                    <em>{watermarkConfig.opacityPct}%</em>
                  </label>
                  <label className="webinar-toggle">
                    <input
                      type="checkbox"
                      aria-label="Show classification banner"
                      checked={watermarkConfig.showClassification}
                      onChange={(e) => setWatermarkConfig((c) => updateWatermark(c, { showClassification: e.target.checked }))}
                    />
                    Classification banner
                  </label>
                  {watermarkConfig.showClassification && (
                    <label className="role-control">
                      <span>Level</span>
                      <select
                        aria-label="Classification level"
                        value={watermarkConfig.classification}
                        onChange={(e) => setWatermarkConfig((c) => updateWatermark(c, { classification: e.target.value as WatermarkConfig["classification"] }))}
                      >
                        {confidentialityLevels.map((level) => (
                          <option key={level} value={level}>{level}</option>
                        ))}
                      </select>
                    </label>
                  )}
                  {spec.classificationBanner && (
                    <div
                      className="watermark-classification"
                      style={{ color: classificationColor(watermarkConfig.classification) }}
                      aria-label="Classification banner"
                    >
                      {spec.classificationBanner}
                    </div>
                  )}
                  {issues.map((issue) => (
                    <p key={issue} className="watermark-issue" role="status">{issue}</p>
                  ))}
                </div>
              );
            })()}
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

          <section className="panel" aria-label="Loudness normalisation">
            <div className="section-title">
              <Gauge size={15} />
              Loudness normalisation
            </div>
            {(() => {
              const integratedLufs = production.audioMix.loudnessLufs;
              const limiterActive = production.audioMix.limiterActive;
              const reading = {
                integratedLufs,
                shortTermLufs: integratedLufs + 1.2,
                truePeakDbtp: limiterActive ? -1.5 : -6,
                loudnessRangeLu: 9,
              };
              const plan = planLoudnessNormalisation(reading, "zoom");
              const { status } = plan;
              return (
                <div className={`loudness-panel level-${status.level}`} aria-label="Loudness status">
                  <div className="loudness-header">
                    <span className="loudness-reading">{formatLufs(integratedLufs)}</span>
                    <span className="loudness-target">target {formatLufs(plan.target.targetLufs)}</span>
                    <span className={`loudness-level level-${status.level}`}>{status.level.replace("-", " ")}</span>
                  </div>
                  <div className="health-grid">
                    <ControlReadout label="True peak" value={formatDbtp(reading.truePeakDbtp)} />
                    <ControlReadout label="Peak limit" value={formatDbtp(plan.limiterCeilingDbtp)} />
                    <ControlReadout label="Gain adjust" value={`${status.gainAdjustmentDb >= 0 ? "+" : ""}${status.gainAdjustmentDb.toFixed(1)} dB`} />
                    <ControlReadout label="Compressor" value={plan.suggestedRatio === 1 ? "Not needed" : `${plan.suggestedRatio}:1`} />
                    <ControlReadout label="Standard" value={plan.target.standard} />
                  </div>
                  {status.warnings.map((w) => (
                    <p key={w} className="multitrack-warning" role="status">{w}</p>
                  ))}
                  <label className="role-control">
                    <span>Target standard</span>
                    <select aria-label="Loudness target" defaultValue="zoom" onChange={() => {}}>
                      {loudnessTargets.map((t) => (
                        <option key={t.id} value={t.id}>{t.name}</option>
                      ))}
                    </select>
                  </label>
                </div>
              );
            })()}
          </section>
        </div>
      </div>

      <div aria-label="Media tab" className={`tab-content ${activeTab === "media" ? "active" : ""}`} hidden={activeTab !== "media"}>
        <div className="tab-panel">
          <section className="panel" aria-label="Program color grade">
            <div className="section-title">
              <Palette size={15} />
              Color grade
            </div>
            <p className="brand-kit-summary">{summarizeColorGrade(production.colorGrade)}</p>
            <label className="role-control">
              <span>LUT preset</span>
              <select
                aria-label="Color LUT"
                onChange={(event) => updateColorGrade({ lut: event.target.value as ProductionState["colorGrade"]["lut"] })}
                value={production.colorGrade.lut}
              >
                {colorGradeLuts.map((lut) => (
                  <option key={lut} value={lut}>{lut}</option>
                ))}
              </select>
            </label>
            {(["exposure", "contrast", "saturation", "temperature"] as const).map((axis) => (
              <label className="gain-control" key={axis}>
                <span>{axis}</span>
                <input
                  aria-label={`Color ${axis}`}
                  max={50}
                  min={-50}
                  onChange={(event) => updateColorGrade({ [axis]: Number(event.target.value) })}
                  step={1}
                  type="range"
                  value={production.colorGrade[axis]}
                />
                <em>{production.colorGrade[axis] > 0 ? `+${production.colorGrade[axis]}` : production.colorGrade[axis]}</em>
              </label>
            ))}
          </section>

          <section className="panel" aria-label="Media bin">
            <div className="section-title">
              <Save size={15} />
              Media bin
            </div>
            <p className="brand-kit-summary">{summarizeMediaBin(production.mediaBin)}</p>
            <div className="media-bin-groups">
              {groupMediaBin(production.mediaBin).map((group) => (
                <div className="media-bin-group" key={group.kind}>
                  <span className="media-bin-kind">{group.label}</span>
                  {group.assets.map((asset) => (
                    <div className="media-bin-asset" key={asset.id}>
                      <strong>{asset.name}</strong>
                      {asset.durationMs !== undefined && <em>{Math.round(asset.durationMs / 100) / 10}s</em>}
                      <button
                        className="icon-button"
                        aria-label={`Remove ${asset.name}`}
                        onClick={() => removeBinAsset(asset.id)}
                      >
                        <X size={13} />
                      </button>
                    </div>
                  ))}
                </div>
              ))}
            </div>
            <div className="add-destination" aria-label="Add media asset">
              <label>
                <span>Asset type</span>
                <select
                  aria-label="Media asset type"
                  onChange={(event) => setMediaAssetKind(event.target.value as MediaAssetKind)}
                  value={mediaAssetKind}
                >
                  {mediaAssetKinds.map((kind) => (
                    <option key={kind} value={kind}>{kind}</option>
                  ))}
                </select>
              </label>
              <button className="ghost-button wide" onClick={addBinAsset}>
                <Plus size={16} />
                Add asset
              </button>
            </div>
          </section>

          <section className="panel" aria-label="Clip trimmer">
            <div className="section-title">
              <Clapperboard size={15} />
              Clip trimmer
            </div>
            {(() => {
              const result = trimClip(clipRegion, clipSourceDurationSec);
              return (
                <div className="clip-trim-panel" aria-label="Clip trim controls">
                  <p className="clip-trim-summary">{summarizeTrim(result, clipRegion.fps)}</p>
                  <div className="health-grid">
                    <ControlReadout label="In" value={formatTimecode(result.region.inPointSec, clipRegion.fps)} />
                    <ControlReadout label="Out" value={formatTimecode(result.region.outPointSec, clipRegion.fps)} />
                    <ControlReadout label="Duration" value={formatTimecode(result.durationSec, clipRegion.fps)} />
                    <ControlReadout label="FPS" value={String(clipRegion.fps)} />
                  </div>
                  {result.warnings.map((w) => (
                    <p className="clip-trim-warning" key={w} role="status">{w}</p>
                  ))}
                  <div className="clip-trim-bar" aria-label="Trim bar">
                    <div
                      className="trim-range"
                      style={{
                        left: `${(result.region.inPointSec / clipSourceDurationSec) * 100}%`,
                        width: `${(result.durationSec / clipSourceDurationSec) * 100}%`,
                      }}
                    />
                    {clipMarkers.map((m) => (
                      <div
                        key={m.id}
                        className="trim-marker"
                        aria-label={`Marker ${m.label}`}
                        style={{
                          left: `${(m.positionSec / clipSourceDurationSec) * 100}%`,
                          background: m.color,
                        }}
                        title={m.label}
                      />
                    ))}
                  </div>
                  <div className="clip-trim-controls">
                    <button
                      className="ghost-button"
                      onClick={() => setClipRegion((r) => rippleTrim(r, { edge: "in", deltaSec: -1, sourceDurationSec: clipSourceDurationSec }).region)}
                    >
                      ← In
                    </button>
                    <button
                      className="ghost-button"
                      onClick={() => setClipRegion((r) => rippleTrim(r, { edge: "in", deltaSec: 1, sourceDurationSec: clipSourceDurationSec }).region)}
                    >
                      In →
                    </button>
                    <button
                      className="ghost-button"
                      onClick={() => setClipRegion((r) => rippleTrim(r, { edge: "out", deltaSec: -1, sourceDurationSec: clipSourceDurationSec }).region)}
                    >
                      ← Out
                    </button>
                    <button
                      className="ghost-button"
                      onClick={() => setClipRegion((r) => rippleTrim(r, { edge: "out", deltaSec: 1, sourceDurationSec: clipSourceDurationSec }).region)}
                    >
                      Out →
                    </button>
                    <button
                      className="ghost-button"
                      onClick={() => setClipMarkers((ms) => addMarker(ms, (clipRegion.inPointSec + clipRegion.outPointSec) / 2, "Cue"))}
                    >
                      Add marker
                    </button>
                  </div>
                </div>
              );
            })()}
          </section>

          <section className="panel" aria-label="Chapter markers">
            <div className="section-title">
              <FileText size={15} />
              Chapter markers
            </div>
            {(() => {
              const warnings = validateChapters(chapterList, "youtube");
              const nowChapter = activeChapterAt(chapterList, elapsedSeconds);
              const exported = chapterList.markers.length > 0 ? exportChapters(chapterList, chapterExportFormat) : "";
              return (
                <div className="chapter-panel" aria-label="Chapter panel">
                  <p className="chapter-summary">{summarizeChapters(chapterList)}</p>
                  {nowChapter && (
                    <p className="chapter-active">Now: <strong>{nowChapter.title}</strong></p>
                  )}
                  <div className="chapter-list">
                    {chapterList.markers.map((m) => (
                      <div key={m.id} className="chapter-row" aria-label={`Chapter ${m.title}`}>
                        <span className="chapter-time">{m.startSec === 0 ? "00:00" : `${Math.floor(m.startSec / 60).toString().padStart(2, "0")}:${(Math.floor(m.startSec) % 60).toString().padStart(2, "0")}`}</span>
                        <span className="chapter-title">{m.title}</span>
                        <button
                          className="icon-button"
                          aria-label={`Remove chapter ${m.title}`}
                          onClick={() => setChapterList((cl) => removeChapter(cl, m.id))}
                        >
                          <X size={12} />
                        </button>
                      </div>
                    ))}
                  </div>
                  {warnings.map((w) => (
                    <p className="chapter-warning" key={w} role="status">{w}</p>
                  ))}
                  <button
                    className="ghost-button wide"
                    onClick={() => setChapterList((cl) => addChapter(cl, Math.floor(elapsedSeconds), `Chapter ${cl.markers.length + 1}`))}
                  >
                    <Plus size={14} />
                    Mark chapter now ({Math.floor(elapsedSeconds)}s)
                  </button>
                  {chapterList.markers.length > 0 && (
                    <div className="chapter-export">
                      <select
                        aria-label="Chapter export format"
                        value={chapterExportFormat}
                        onChange={(e) => setChapterExportFormat(e.target.value as typeof chapterExportFormat)}
                      >
                        <option value="youtube">YouTube</option>
                        <option value="vtt">WebVTT</option>
                        <option value="text">Text</option>
                        <option value="json">JSON</option>
                      </select>
                      <pre className="chapter-export-preview" aria-label="Chapter export preview">{exported}</pre>
                    </div>
                  )}
                </div>
              );
            })()}
          </section>

          {selectedParticipant && (
            <section className="panel detail-panel">
              <div className="section-title">
                <Video size={15} />
                Smart handling - media
              </div>
              <h2>{selectedParticipant.name}</h2>
              <p>{selectedParticipant.title}</p>
              {selectedAutoCrop && (
                <div className="auto-crop-panel" aria-label="Face-aware auto-crop">
                  <p className="brand-kit-summary">{describeFraming(selectedAutoCrop)}</p>
                  <div className="health-grid">
                    <ControlReadout label="Auto framing" value={selectedAutoCrop.framingLabel} />
                    <ControlReadout label="Recommended zoom" value={`${selectedAutoCrop.zoom.toFixed(2)}x`} />
                    <ControlReadout label="Headroom" value={`${selectedAutoCrop.headroomPercent}%`} />
                    <ControlReadout label="Feed quality" value={selectedAutoCrop.quality} />
                  </div>
                  {selectedAutoCrop.warning && (
                    <p className="auto-crop-warning" role="status">{selectedAutoCrop.warning}</p>
                  )}
                </div>
              )}
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

          <section className="panel" aria-label="Scene intelligence">
            <div className="section-title">
              <Sparkles size={15} />
              Scene intelligence
            </div>
            {(() => {
              const signal = {
                videoOnCount: production.participants.filter((p) => p.health !== "video-off").length,
                screenShareActive,
                hasActiveSpeaker: production.participants.some((p) => p.isActiveSpeaker),
                hostVideoOn: production.participants.some((p) => p.role === "Host" && p.health !== "video-off"),
                isOutro: production.scenes.some((s: SceneTemplate) => s.layout === "outro" && s.id === production.activeSceneId),
              };
              const rec = recommendScene(signal);
              const automationOn = production.mode === "set-and-forget";
              const autoDecision = decideAutoSwitch(
                (production.scenes.find((s: SceneTemplate) => s.id === production.activeSceneId)?.layout ?? "smart-grid") as SceneLayout,
                signal,
                0
              );
              return (
                <div className="scene-intelligence-panel" aria-label="Scene intelligence panel">
                  <div className="scene-intel-summary">
                    {summarizeSceneIntelligence(signal, automationOn)}
                  </div>
                  <div className="health-grid">
                    <ControlReadout label="Recommended" value={rec.sceneName} />
                    <ControlReadout label="Layout" value={rec.layout} />
                    <ControlReadout label="Confidence" value={rec.confidence} />
                    <ControlReadout label="Cameras on" value={String(signal.videoOnCount)} />
                    <ControlReadout label="Screen share" value={signal.screenShareActive ? "Active" : "Off"} />
                    <ControlReadout label="Auto-switch" value={autoDecision.shouldSwitch ? "Now" : autoDecision.holdUntilMs > 0 ? `Hold ${(autoDecision.holdUntilMs / 1000).toFixed(0)}s` : "Stable"} />
                  </div>
                  <p className="scene-intel-reason">{rec.reason}</p>
                </div>
              );
            })()}
          </section>

          <section className="panel" aria-label="Show clock">
            <div className="section-title">
              <Activity size={15} />
              Show clock
            </div>
            {(() => {
              const sceneNames = production.scenes.map((s: SceneTemplate) => s.name);
              const rundown = buildRundownFromScenes(sceneNames, Math.ceil(sceneNames.length * 8));
              const clock = computeShowClock(rundown, showClockSegmentIndex, showClockSegmentElapsed, showClockTotalElapsed);
              return (
                <div className="show-clock-panel">
                  <div className="show-clock-display" aria-label="Show clock display">
                    <span className="show-clock-elapsed">{clock.elapsedLabel}</span>
                    <span className={`show-clock-remaining ${clock.showOver ? "over" : ""}`}>
                      {clock.remainingLabel}
                    </span>
                    <span className="show-clock-total">{formatClock(clock.totalPlannedSeconds)} planned</span>
                  </div>
                  <div className="show-clock-segments">
                    {clock.segments.map((seg, i) => (
                      <button
                        key={seg.segmentId}
                        className={`show-clock-seg status-${seg.status}`}
                        onClick={() => {
                          setShowClockSegmentIndex(i);
                          setShowClockSegmentElapsed(0);
                        }}
                        aria-label={`${seg.name} segment`}
                      >
                        <strong>{seg.name}</strong>
                        <span>{formatClock(seg.plannedSeconds)}</span>
                        <span className={`seg-status status-${seg.status}`}>{seg.status}</span>
                      </button>
                    ))}
                  </div>
                  <div className="show-clock-controls">
                    <button
                      className="ghost-button"
                      disabled={showClockSegmentIndex < 0}
                      onClick={() => setShowClockSegmentElapsed((s) => s + 60)}
                    >
                      +1 min
                    </button>
                    <button
                      className="ghost-button"
                      onClick={() => {
                        setShowClockSegmentIndex(-1);
                        setShowClockSegmentElapsed(0);
                        setShowClockTotalElapsed(0);
                      }}
                    >
                      Reset
                    </button>
                    <button
                      className="ghost-button"
                      disabled={showClockSegmentIndex >= rundown.length - 1}
                      onClick={() => {
                        const next = showClockSegmentIndex + 1;
                        setShowClockSegmentIndex(next);
                        setShowClockSegmentElapsed(0);
                        setShowClockTotalElapsed((t) => t + (rundown[showClockSegmentIndex]?.plannedSeconds ?? 0));
                      }}
                    >
                      Next segment
                    </button>
                  </div>
                </div>
              );
            })()}
          </section>

          <section className="panel" aria-label="Pre-show countdown">
            <div className="section-title">
              <Activity size={15} />
              Pre-show countdown
            </div>
            {(() => {
              const now = 0;
              const show = {
                scheduledAtUnixSec: preShowSecondsToLive,
                lobbyDurationSec: 600,
                showTitle: production.meetingTitle || "Upcoming Show",
              };
              const countdown = computeCountdown(now, show);
              const cues = buildPreShowCues(now, show);
              return (
                <div className="pre-show-panel" aria-label="Pre-show countdown panel">
                  <div className="pre-show-display">
                    <span className="pre-show-tminus" aria-label="T-minus label">{formatTMinus(countdown.secondsToLive)}</span>
                    <span className={`pre-show-phase phase-${countdown.phase}`}>{countdown.phase}</span>
                  </div>
                  <p className="pre-show-summary">{countdown.summary}</p>
                  {countdown.hotWarning && (
                    <p className="pre-show-warning">{countdown.hotWarning}</p>
                  )}
                  <div className="pre-show-cues" aria-label="Pre-show cues">
                    {cues.map((cue) => (
                      <div className="pre-show-cue" key={cue.label}>
                        <span className="cue-label">{cue.label}</span>
                        <span className="cue-time">{cue.secondsFromNow >= 0 ? `in ${Math.round(cue.secondsFromNow)}s` : `${Math.round(-cue.secondsFromNow)}s ago`}</span>
                      </div>
                    ))}
                  </div>
                  <div className="pre-show-controls">
                    <button
                      className="ghost-button"
                      onClick={() => setPreShowSecondsToLive((s) => Math.max(0, s - 300))}
                    >
                      -5 min
                    </button>
                    <button
                      className="ghost-button"
                      onClick={() => setPreShowSecondsToLive((s) => s + 300)}
                    >
                      +5 min
                    </button>
                    <button
                      className="ghost-button"
                      disabled={countdown.phase === "live"}
                      onClick={() => {
                        const liveState = markShowLive(countdown);
                        void liveState;
                        setPreShowSecondsToLive(0);
                      }}
                    >
                      Go Live
                    </button>
                    <button
                      className="ghost-button"
                      disabled={countdown.phase !== "live"}
                      onClick={() => {
                        const overState = markShowOver(countdown);
                        void overState;
                        setPreShowSecondsToLive(-1);
                      }}
                    >
                      End Show
                    </button>
                  </div>
                </div>
              );
            })()}
          </section>

          <section className="panel" aria-label="Control surface">
            <div className="section-title">
              <Settings2 size={15} />
              Control surface
            </div>
            <p className="brand-kit-summary">{summarizeControlSurface()}</p>
            <div className="control-surface-grid">
              {controlSurface.map((button) => {
                const status = controlButtonStatus(button.action, {
                  recording: production.recording,
                  streaming: production.streaming,
                  automation: production.mode === "set-and-forget",
                  inMeeting: meetingState === "in_meeting"
                });
                return (
                  <button
                    aria-label={`Control ${button.label}`}
                    className={`control-key ${status.active ? "active" : ""}`}
                    disabled={status.disabled}
                    key={button.id}
                    onClick={() => triggerControlAction(button.action)}
                  >
                    <strong>{button.label}</strong>
                    <em>{status.stateLabel}</em>
                  </button>
                );
              })}
            </div>
          </section>

          <section className="panel">
            <div className="section-title">
              <Clapperboard size={15} />
              Transition
            </div>
            <div className="transition-picker" aria-label="Transition controls">
              {transitionStyles.map((style) => (
                <button
                  className={production.transition.style === style ? "selected" : ""}
                  key={style}
                  onClick={() => setTransitionStyle(style)}
                >
                  {style}
                </button>
              ))}
            </div>
            <p className="transition-descriptor">{describeTransition(production.transition.style, production.transition.durationMs).summary}</p>
            <p className="transition-status">{production.transition.statusText}</p>
          </section>

          <section className="panel" aria-label="Stinger presets">
            <div className="section-title">
              <Sparkles size={15} />
              Stinger presets
            </div>
            {(() => {
              const presets = stingerPresets();
              const activeStingerId = production.transition.style === "stinger" ? (presets[0]?.id ?? null) : null;
              const activePreset = activeStingerId ? getStingerPreset(activeStingerId) : null;
              return (
                <div className="stinger-list" aria-label="Stinger list">
                  {presets.map((preset) => (
                    <div
                      key={preset.id}
                      className={`stinger-row ${preset.id === activeStingerId ? "active" : ""}`}
                      aria-label={`Stinger ${preset.name}`}
                    >
                      <span className="stinger-name">{preset.name}</span>
                      <span className="stinger-desc">{describeStinger(preset)}</span>
                    </div>
                  ))}
                  {activePreset && (
                    <div className="stinger-active-detail">
                      <ControlReadout label="In" value={`${activePreset.inDurationMs}ms`} />
                      <ControlReadout label="Hold" value={`${activePreset.holdDurationMs}ms`} />
                      <ControlReadout label="Out" value={`${activePreset.outDurationMs}ms`} />
                      <ControlReadout label="Switch point" value={activePreset.sceneSwitchPoint} />
                    </div>
                  )}
                </div>
              );
            })()}
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

          {aiStudio && (
            <section className="panel ai-studio-panel">
              <div className="section-title">
                <FileText size={15} />
                AI Studio
              </div>
              <div className="ai-show-notes" aria-label="AI Studio show notes">
                <strong>{aiStudio.showNotes.title}</strong>
                <p>{aiStudio.showNotes.summary}</p>
                <div className="ai-chip-row">
                  <span>{aiStudio.transcript.length} segment</span>
                  <span>{aiStudio.chapters.length} chapter</span>
                  <span>{aiStudio.highlights.length} highlights</span>
                </div>
              </div>
              <div className="ai-list" aria-label="AI Studio highlights">
                {aiStudio.highlights.slice(0, 2).map((highlight) => (
                  <div key={highlight.id}>
                    <strong>{highlight.title}</strong>
                    <span>{highlight.suggestedClipName}</span>
                    <em>{highlight.confidence}%</em>
                  </div>
                ))}
              </div>
              <div className="ai-list" aria-label="AI Studio next actions">
                {aiStudio.showNotes.nextActions.slice(0, 3).map((action) => (
                  <div key={action}>
                    <span>{action}</span>
                  </div>
                ))}
              </div>
            </section>
          )}
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
          <OutputStat label="Program" value={programResLabel} health={production.output.network} />
          <OutputStat
            label="Stream"
            value={production.streaming ? `${programResLabel} ${production.output.bitrateMbps} Mbps` : "Idle"}
            health={production.streaming ? production.output.network : "idle"}
          />
          <OutputStat
            label="Record"
            value={production.recording ? programResLabel : "Idle"}
            health={production.recording ? "good" : "idle"}
          />
          <div className="mini-stats">
            <MiniStat icon={<Gauge size={14} />} label="CPU" value={cpuLoad} />
            <MiniStat icon={<Activity size={14} />} label="Memory" value={memoryLoad} />
            <MiniStat icon={<HardDrive size={14} />} label="Disk" value={diskLoad} />
            <div className="stat-readout">
              <span className="stat-label">Frame Drops</span>
              <strong>{production.output.droppedFrames} (0.0%)</strong>
            </div>
            <div className="stat-readout">
              <span className="stat-label">Live</span>
              <strong>{formatElapsed(production.outputSession.elapsedSeconds)}</strong>
            </div>
          </div>
          <div className="master-audio">
            <div className="master-audio-header">
              MASTER AUDIO
              <span className="db-scale" aria-hidden="true">
                {[-60, -48, -36, -24, -12, -6, -3, 0].map((tick) => (
                  <span key={tick}>{tick}</span>
                ))}
              </span>
            </div>
            <div className="master-meter" aria-label="Master audio levels">
              <span className="meter-channel">L</span>
              <div className="meter-bar">
                <span className="meter-fill" style={{ width: `${masterLevel}%` }} />
              </div>
            </div>
            <div className="master-meter" aria-label="Master audio levels right">
              <span className="meter-channel">R</span>
              <div className="meter-bar">
                <span className="meter-fill" style={{ width: `${Math.max(0, masterLevel - 4)}%` }} />
              </div>
            </div>
          </div>
          <div className="master-volume">
            <Volume2 size={16} />
            <span>0.0 dB</span>
            <Settings2 size={14} className="decorative-icon" aria-hidden="true" />
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
      <svg className="mini-stat-spark" viewBox="0 0 60 16" preserveAspectRatio="none" aria-hidden="true">
        <polyline points={sparklinePoints(value, label)} />
      </svg>
    </div>
  );
}

function sparklinePoints(value: number, seed: string) {
  const seedNum = seed.split("").reduce((total, char) => total + char.charCodeAt(0), 0);
  const samples = 12;
  return Array.from({ length: samples }, (_, index) => {
    const wave = Math.sin((index / samples) * Math.PI * 2 + seedNum) * 9;
    const point = Math.min(100, Math.max(0, value + wave));
    const x = (index / (samples - 1)) * 60;
    const y = 16 - (point / 100) * 14 - 1;
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(" ");
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

function OutputStat({
  label,
  value,
  health
}: {
  label: string;
  value: string;
  health: OutputHealth["network"] | "idle";
}) {
  const healthLabel = health === "idle" ? "Idle" : health === "warning" ? "Warning" : "Good";
  return (
    <div className="output-stat">
      <span className="output-stat-label">{label}</span>
      <strong className="output-stat-value">{value}</strong>
      <span className={`output-stat-health health-${health}`}>{healthLabel}</span>
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

function shortResolutionLabel(resolution: string, fps: number) {
  const height = Number(resolution.split("x")[1]);
  if (!Number.isFinite(height)) {
    return `${resolution} ${fps}fps`;
  }

  const tier = height >= 2160 ? "4K" : `${height}p`;
  return `${tier}${fps}`;
}

function filterMediaCoreEvents(events: NativeMediaCoreEvent[], filter: DiagnosticsFilter) {
  const sorted = [...events].sort((left, right) => right.atMs - left.atMs);

  if (filter === "critical") {
    return sorted.filter((event) => event.severity === "critical");
  }

  if (filter === "warning") {
    return sorted.filter((event) => event.severity === "warning");
  }

  if (filter === "recovery") {
    return sorted.filter((event) => event.severity === "info" || /recover/i.test(`${event.title} ${event.detail}`));
  }

  return sorted;
}

function formatEventTime(atMs: number) {
  const seconds = Math.max(0, Math.floor(atMs / 1000));
  return formatElapsed(seconds);
}

function isEditableTarget(target: EventTarget | null) {
  if (!(target instanceof HTMLElement)) {
    return false;
  }

  return Boolean(target.closest("input, textarea, select, [contenteditable='true']"));
}
