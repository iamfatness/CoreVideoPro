export type ParticipantRole = "Host" | "Guest" | "Presenter" | "Panelist";

export type FeedHealth = "live" | "low-resolution" | "recovering" | "video-off";

export type MediaFrameState = {
  sourceId: string;
  participantId?: string;
  kind: "participant-video" | "screen-share";
  frameId: number;
  width: number;
  height: number;
  fps: number;
  ageMs: number;
  crop: {
    x: number;
    y: number;
    width: number;
    height: number;
  };
  dominantColor: string;
  label: string;
  health: FeedHealth;
};

export type Participant = {
  id: string;
  name: string;
  title: string;
  role: ParticipantRole;
  breakoutRoomId: string;
  breakoutRoomName: string;
  isActiveSpeaker: boolean;
  isMuted: boolean;
  isScreenSharing: boolean;
  audioLevel: number;
  gainDb: number;
  noiseSuppression: boolean;
  cropConfidence: number;
  health: FeedHealth;
};

export type CaptureDeviceVendor = "blackmagic" | "aja";

export type CaptureDeviceConnectionState =
  | "disconnected"
  | "detected"
  | "connected"
  | "format-mismatch"
  | "error";

export type CaptureDeviceInput = {
  id: string;
  label: string;
  hasEmbeddedAudio: boolean;
};

export type CaptureDeviceState = {
  id: string;
  vendor: CaptureDeviceVendor;
  name: string;
  inputs: CaptureDeviceInput[];
  selectedInputId: string;
  resolution: { width: number; height: number };
  frameRate: number;
  connectionState: CaptureDeviceConnectionState;
  signalPresent: boolean;
  droppedFrames: number;
  audioSyncOffsetMs: number;
};

export type ParticipantVideoEffect = {
  participantId: string;
  cropMode: "auto" | "manual";
  manualZoom: number;
  chromaKeyEnabled: boolean;
  chromaKeyColor: "green" | "blue";
  spillSuppression: number;
};

export type ParticipantAudioMix = {
  participantId: string;
  inputLevel: number;
  outputLevel: number;
  gainDb: number;
  manualGainDb?: number;
  noiseSuppression: boolean;
  limiterActive: boolean;
  muted: boolean;
  status: "balanced" | "boosting" | "ducking" | "muted";
};

export type AudioMixState = {
  participants: ParticipantAudioMix[];
  masterLevel: number;
  loudnessLufs: number;
  limiterActive: boolean;
  summary: string;
};

export type CaptionFontSize = "small" | "medium" | "large";

export type CaptionStyle = {
  fontSize: CaptionFontSize;
  textColor: string;
  backgroundOpacity: number;
  uppercase: boolean;
};

export type CaptionOverlayState = {
  text: string;
  speakerName: string;
  confidence: number;
  latencyMs: number;
  captionPosition: "bottom" | "top";
  lowerThirdPosition: "lower-left" | "upper-left";
  warnings: string[];
  adaptiveSummary: string;
};

export type CaptionTranscriptEntry = {
  id: string;
  speakerName: string;
  role: string;
  text: string;
  confidence: number;
  atSeconds: number;
};

export type GraphicOverlay = {
  id: string;
  name: string;
  kind: "bug" | "banner" | "cta";
  text: string;
  position: "top-right" | "bottom-right" | "center";
  accent: string;
  enabled: boolean;
};

export type BrandKitFont = "Inter" | "Poppins" | "Roboto" | "Georgia";

export type BrandKit = {
  name: string;
  logoText: string;
  brandColor: string;
  accentColor: string;
  backgroundColor: string;
  backgroundImageUrl: string;
  fontFamily: BrandKitFont;
  lowerThirdStyle: "solid" | "minimal" | "gradient";
};

export type ColorGradeLut = "none" | "neutral" | "warm-film" | "cool-broadcast" | "punch";

export type ColorGrade = {
  lut: ColorGradeLut;
  exposure: number;
  contrast: number;
  saturation: number;
  temperature: number;
};

export type VirtualCameraState = {
  enabled: boolean;
  deviceName: string;
  mirrored: boolean;
};

export type MediaAssetKind = "stinger" | "lower-third" | "audio-bed" | "slate";

export type MediaAsset = {
  id: string;
  name: string;
  kind: MediaAssetKind;
  durationMs?: number;
};

export type SourceRoute = {
  id: string;
  mode: "fixed" | "active-speaker" | "spotlight" | "screen-share" | "none";
  participantId?: string;
  spotlightIndex?: number;
  audioRole: "mix" | "isolated" | "audience";
};

export type SceneTemplate = {
  id: string;
  name: string;
  type: "intro" | "interview" | "slides" | "panel" | "closing";
  layout: "host-focus" | "two-up" | "speaker-slides" | "smart-grid" | "outro";
  automation: string;
  slots?: string[];
  routes?: SourceRoute[];
  selected?: boolean;
};

export type TransitionState = {
  style: "cut" | "fade" | "slide" | "wipe" | "stinger";
  durationMs: number;
  statusText: string;
  lastTakenSceneId?: string;
};

export type AutoProductionState = {
  recommendedSceneId: string;
  confidence: number;
  reason: string;
  action: "hold" | "queue" | "take";
  lastAppliedSceneId?: string;
};

export type OutputHealth = {
  resolution: string;
  fps: number;
  bitrateMbps: number;
  droppedFrames: number;
  encoderLoad: number;
  network: "excellent" | "good" | "warning";
  /** Available disk space on the recording volume in GB */
  availableDiskGb: number;
};

export type OutputProfile = {
  id: string;
  name: string;
  resolution: string;
  fps: number;
  targetBitrateMbps: number;
};

export type OutputDestination = {
  id: string;
  name: string;
  protocol: "RTMP" | "NDI" | "SRT" | "WebRTC";
  enabled: boolean;
  latencyMs: number;
  endpoint: string;
  streamKey?: string;
};

export type OutputDestinationState = OutputDestination & {
  active: boolean;
  health: "idle" | "live" | "warning";
  bitrateMbps: number;
};

export type RecordingSettings = {
  folder: string;
  filenamePrefix: string;
  format: "mp4" | "mov" | "mkv";
  quality: "standard" | "high" | "archive";
  isoParticipantIds: string[];
};

export type OutputSessionState = {
  recording: boolean;
  streaming: boolean;
  recordingSettings?: RecordingSettings;
  recordingFile?: string;
  streamDestinationId?: string;
  activeDestinationIds: string[];
  destinations: OutputDestinationState[];
  elapsedSeconds: number;
  health: OutputHealth;
  statusText: string;
};

export type TranscriptSegment = {
  id: string;
  startSeconds: number;
  endSeconds: number;
  speakerName: string;
  text: string;
  confidence: number;
};

export type AiChapter = {
  id: string;
  title: string;
  startSeconds: number;
  summary: string;
};

export type AiHighlight = {
  id: string;
  title: string;
  summary: string;
  startSeconds: number;
  endSeconds: number;
  speakerName: string;
  confidence: number;
  suggestedClipName: string;
};

export type AiShowNotes = {
  title: string;
  summary: string;
  bullets: string[];
  nextActions: string[];
};

export type AiStudioState = {
  transcript: TranscriptSegment[];
  chapters: AiChapter[];
  highlights: AiHighlight[];
  showNotes: AiShowNotes;
  rundown: string[];
};

export type ProductionState = {
  meetingTitle: string;
  mode: "manual" | "set-and-forget";
  activeSceneId: string;
  previewSceneId: string;
  selectedBreakoutRoomId: string;
  transition: TransitionState;
  autoProduction: AutoProductionState;
  magicSceneStatus: string;
  recording: boolean;
  streaming: boolean;
  outputSession: OutputSessionState;
  recordingSettings: RecordingSettings;
  selectedOutputProfileId: string;
  outputProfiles: OutputProfile[];
  outputDestinations: OutputDestination[];
  audioMix: AudioMixState;
  captionOverlay: CaptionOverlayState;
  captionStyle: CaptionStyle;
  captionTranscript: CaptionTranscriptEntry[];
  graphics: GraphicOverlay[];
  brandKit: BrandKit;
  colorGrade: ColorGrade;
  virtualCamera: VirtualCameraState;
  mediaBin: MediaAsset[];
  videoEffects: ParticipantVideoEffect[];
  captions: string;
  participants: Participant[];
  mediaFrames: MediaFrameState[];
  scenes: SceneTemplate[];
  output: OutputHealth;
  captureDevices: CaptureDeviceState[];
};

export type ShowPreset = {
  id: string;
  name: string;
  savedAt: string;
  meetingTitle: string;
  mode: ProductionState["mode"];
  activeSceneId: string;
  previewSceneId: string;
  transition: Pick<TransitionState, "style" | "durationMs">;
  selectedOutputProfileId: string;
  outputProfiles: OutputProfile[];
  recordingSettings: RecordingSettings;
  scenes: SceneTemplate[];
  graphics: GraphicOverlay[];
  videoEffects: ParticipantVideoEffect[];
  outputDestinations: OutputDestination[];
};

export type PresetSummary = {
  id: string;
  name: string;
  savedAt: string;
  sceneCount: number;
  armedDestinationCount: number;
  enabledGraphicCount: number;
};

export type SupportBundleMediaCore = {
  sceneId?: string;
  renderPlanId?: string;
  source: {
    adapterId: string;
    kind: "zoom-sdk" | "local-camera" | "test-pattern";
    status: "idle" | "subscribed" | "degraded" | "failed";
    subscribedSourceCount: number;
    droppedFrameCount: number;
    lowResolutionFrameCount: number;
    issues: Array<{
      sourceId: string;
      participantId?: string;
      displayName?: string;
      health: FeedHealth;
      severity: "warning" | "critical";
      detail: string;
    }>;
  };
  compositor: {
    status: "idle" | "live" | "degraded" | "failed";
    programFrameCount: number;
    droppedFrameCount: number;
    degradedFrameCount: number;
  };
  transport: {
    status: "idle" | "publishing" | "degraded";
    frameNumber?: number;
    latencyMs: number;
  };
  encoder: {
    status: "idle" | "encoding" | "warning" | "failed";
    lifecycle: "idle" | "prepared" | "encoding" | "stopped" | "failed";
    targetCount: number;
  };
  senders: {
    status: "idle" | "live" | "warning" | "failed";
    activeSenderCount: number;
    destinations: Array<{
      destination: "rtmp" | "ndi" | "srt" | "webrtc";
      status: "idle" | "starting" | "live" | "warning" | "stopped" | "failed";
      framesSent: number;
      retryCount: number;
      bitrateMbps: number;
    }>;
  };
  recording?: {
    status: "recording" | "warning" | "stopped" | "failed";
    writerStatus: "writing" | "warning" | "stopped" | "failed";
    totalFramesWritten: number;
    totalDroppedFrames: number;
    estimatedDiskRateMBps: number;
  };
  operatorActions: Array<{
    actionId: string;
    severity: "info" | "warning" | "critical";
    area: "source" | "routing" | "program" | "recording" | "sender" | "encoder";
    title: string;
    detail: string;
    command?: string;
    relatedId?: string;
  }>;
  eventLog: Array<{
    eventId: string;
    atMs: number;
    severity: "info" | "warning" | "critical";
    area: "source" | "routing" | "program" | "recording" | "sender" | "encoder" | "system";
    title: string;
    detail: string;
    relatedId?: string;
    commandType?: string;
  }>;
  warnings: string[];
};

export type SupportBundle = {
  id: string;
  createdAt: string;
  app: {
    name: string;
    version: string;
    platform: "mock-desktop";
  };
  summaryText: string;
  triageLines: string[];
  production: {
    meetingTitle: string;
    mode: ProductionState["mode"];
    activeSceneId: string;
    previewSceneId: string;
    selectedBreakoutRoomId: string;
    recording: boolean;
    streaming: boolean;
  };
  participants: Array<{
    id: string;
    name: string;
    role: ParticipantRole;
    health: FeedHealth;
    breakoutRoomName: string;
    isActiveSpeaker: boolean;
    isScreenSharing: boolean;
    muted: boolean;
    recommendedAction: string;
  }>;
  output: {
    health: OutputHealth;
    activeDestinationIds: string[];
    destinations: Array<{
      id: string;
      name: string;
      protocol: OutputDestination["protocol"];
      enabled: boolean;
      active: boolean;
      health: OutputDestinationState["health"];
      bitrateMbps: number;
      endpoint: string;
      streamKey: string;
    }>;
    statusText: string;
  };
  actionCounts: {
    duplicateAssignments: number;
    unavailableScreenShare: number;
    lowDeliveredResolution: number;
    sdkSubscribeErrors: number;
  };
  isoCapacity: {
    selectedParticipantIds: string[];
    estimatedPathCount: number;
    estimatedDiskRateMBps: number;
    freeDiskBytes: number;
    recordingRunwayMinutes: number;
    warning?: string;
  };
  mediaCore?: SupportBundleMediaCore;
  warnings: string[];
};

export const initialParticipants: Participant[] = [
  {
    id: "p1",
    name: "Maya Chen",
    title: "Host",
    role: "Host",
    breakoutRoomId: "main",
    breakoutRoomName: "Main room",
    isActiveSpeaker: false,
    isMuted: false,
    isScreenSharing: false,
    audioLevel: 64,
    gainDb: 1.5,
    noiseSuppression: true,
    cropConfidence: 96,
    health: "live"
  },
  {
    id: "p2",
    name: "Andre Wallace",
    title: "Product Lead",
    role: "Presenter",
    breakoutRoomId: "main",
    breakoutRoomName: "Main room",
    isActiveSpeaker: true,
    isMuted: false,
    isScreenSharing: true,
    audioLevel: 82,
    gainDb: -0.5,
    noiseSuppression: true,
    cropConfidence: 93,
    health: "live"
  },
  {
    id: "p3",
    name: "Priya Shah",
    title: "Customer Panelist",
    role: "Panelist",
    breakoutRoomId: "customer-panel",
    breakoutRoomName: "Customer panel",
    isActiveSpeaker: false,
    isMuted: true,
    isScreenSharing: false,
    audioLevel: 18,
    gainDb: 3,
    noiseSuppression: true,
    cropConfidence: 88,
    health: "low-resolution"
  },
  {
    id: "p4",
    name: "Noah Kim",
    title: "Guest Analyst",
    role: "Guest",
    breakoutRoomId: "customer-panel",
    breakoutRoomName: "Customer panel",
    isActiveSpeaker: false,
    isMuted: false,
    isScreenSharing: false,
    audioLevel: 41,
    gainDb: 2,
    noiseSuppression: true,
    cropConfidence: 91,
    health: "live"
  },
  {
    id: "p5",
    name: "Jeremy Collins",
    title: "Engineering Panelist",
    role: "Panelist",
    breakoutRoomId: "main",
    breakoutRoomName: "Main room",
    isActiveSpeaker: false,
    isMuted: false,
    isScreenSharing: false,
    audioLevel: 38,
    gainDb: 1,
    noiseSuppression: true,
    cropConfidence: 90,
    health: "live"
  },
  {
    id: "p6",
    name: "Ava Patel",
    title: "Customer Panelist",
    role: "Panelist",
    breakoutRoomId: "main",
    breakoutRoomName: "Main room",
    isActiveSpeaker: false,
    isMuted: false,
    isScreenSharing: false,
    audioLevel: 34,
    gainDb: 1.5,
    noiseSuppression: true,
    cropConfidence: 89,
    health: "live"
  },
  {
    id: "p7",
    name: "Michael Thompson",
    title: "Attendee",
    role: "Guest",
    breakoutRoomId: "main",
    breakoutRoomName: "Main room",
    isActiveSpeaker: false,
    isMuted: true,
    isScreenSharing: false,
    audioLevel: 8,
    gainDb: 0,
    noiseSuppression: true,
    cropConfidence: 87,
    health: "live"
  },
  {
    id: "p8",
    name: "Linda Park",
    title: "Attendee",
    role: "Guest",
    breakoutRoomId: "main",
    breakoutRoomName: "Main room",
    isActiveSpeaker: false,
    isMuted: false,
    isScreenSharing: false,
    audioLevel: 30,
    gainDb: 0.5,
    noiseSuppression: true,
    cropConfidence: 88,
    health: "live"
  }
];

export function hasActiveScreenShare(participants: Participant[]) {
  return participants.some((participant) => participant.isScreenSharing);
}

export function getActiveSpeaker(participants: Participant[]) {
  return participants.find((participant) => participant.isActiveSpeaker && participant.health !== "video-off");
}

export function sortParticipantsForProduction(participants: Participant[]) {
  const roleRank: Record<ParticipantRole, number> = {
    Host: 0,
    Presenter: 1,
    Guest: 2,
    Panelist: 3
  };

  return [...participants].sort((left, right) => {
    if (left.isActiveSpeaker !== right.isActiveSpeaker) {
      return left.isActiveSpeaker ? -1 : 1;
    }

    if (left.isScreenSharing !== right.isScreenSharing) {
      return left.isScreenSharing ? -1 : 1;
    }

    return roleRank[left.role] - roleRank[right.role];
  });
}

export function getBreakoutRooms(participants: Participant[]) {
  const rooms = new Map<string, { id: string; name: string; participantCount: number }>();

  participants.forEach((participant) => {
    const existing = rooms.get(participant.breakoutRoomId);
    rooms.set(participant.breakoutRoomId, {
      id: participant.breakoutRoomId,
      name: participant.breakoutRoomName,
      participantCount: (existing?.participantCount ?? 0) + 1
    });
  });

  return [...rooms.values()].sort((left, right) => {
    if (left.id === "main") {
      return -1;
    }

    if (right.id === "main") {
      return 1;
    }

    return left.name.localeCompare(right.name);
  });
}

export const initialProduction: ProductionState = {
  meetingTitle: "AI Product Launch Webinar",
  mode: "set-and-forget",
  activeSceneId: "speaker-slides",
  previewSceneId: "speaker-slides",
  selectedBreakoutRoomId: "all",
  transition: {
    style: "fade",
    durationMs: 420,
    statusText: "Program ready"
  },
  autoProduction: {
    recommendedSceneId: "speaker-slides",
    confidence: 92,
    reason: "Presenter is sharing slides, so keep speaker plus slides live.",
    action: "hold"
  },
  magicSceneStatus: "Ready to rebuild from 4 participants, 1 presenter, screen share active",
  recording: false,
  streaming: false,
  outputSession: {
    recording: false,
    streaming: false,
    activeDestinationIds: [],
    destinations: [],
    elapsedSeconds: 0,
    health: {
      resolution: "1920x1080",
      fps: 60,
      bitrateMbps: 0,
      droppedFrames: 0,
      encoderLoad: 18,
      network: "good",
      availableDiskGb: 247.3
    },
    statusText: "Outputs idle"
  },
  recordingSettings: {
    folder: "Recordings/CoreVideo Pro",
    filenamePrefix: "AI_Product_Launch_Webinar",
    format: "mp4",
    quality: "high",
    isoParticipantIds: ["p1", "p2"]
  },
  selectedOutputProfileId: "1080p60",
  outputProfiles: [
    { id: "1080p30", name: "1080p 30", resolution: "1920x1080", fps: 30, targetBitrateMbps: 6 },
    { id: "1080p60", name: "1080p 60", resolution: "1920x1080", fps: 60, targetBitrateMbps: 8.2 },
    { id: "4k30", name: "4K 30", resolution: "3840x2160", fps: 30, targetBitrateMbps: 18 },
    { id: "4k60", name: "4K 60", resolution: "3840x2160", fps: 60, targetBitrateMbps: 28 }
  ],
  outputDestinations: [
    {
      id: "custom-rtmp",
      name: "Custom RTMP",
      protocol: "RTMP",
      enabled: true,
      latencyMs: 2100,
      endpoint: "rtmp://live.example.com/app",
      streamKey: "corevideo-demo"
    },
    {
      id: "youtube-primary",
      name: "YouTube",
      protocol: "RTMP",
      enabled: true,
      latencyMs: 2400,
      endpoint: "rtmp://a.rtmp.youtube.com/live2",
      streamKey: "yt-demo-key"
    },
    {
      id: "ndi-program",
      name: "NDI Program",
      protocol: "NDI",
      enabled: false,
      latencyMs: 80,
      endpoint: "CoreVideo Pro Program"
    },
    {
      id: "srt-backup",
      name: "SRT Backup",
      protocol: "SRT",
      enabled: false,
      latencyMs: 420,
      endpoint: "srt://backup.example.com:9000?mode=caller",
      streamKey: "backup-passphrase"
    }
  ],
  audioMix: {
    participants: [],
    masterLevel: 72,
    loudnessLufs: -16,
    limiterActive: false,
    summary: "Smart audio leveling ready"
  },
  captionOverlay: {
    text: "The active speaker is framed automatically while the slides remain readable.",
    speakerName: "Andre Wallace",
    confidence: 94,
    latencyMs: 180,
    captionPosition: "bottom",
    lowerThirdPosition: "upper-left",
    warnings: [],
    adaptiveSummary: "Captions clear; lower-third lifted away from captions"
  },
  captionStyle: {
    fontSize: "medium",
    textColor: "#f7fbf8",
    backgroundOpacity: 70,
    uppercase: false
  },
  captionTranscript: [
    {
      id: "cc-1",
      speakerName: "Maya Chen",
      role: "Host",
      text: "Welcome to the AI Product Launch Webinar.",
      confidence: 96,
      atSeconds: 4
    },
    {
      id: "cc-2",
      speakerName: "Andre Wallace",
      role: "Presenter",
      text: "Let me share the roadmap slides now.",
      confidence: 93,
      atSeconds: 12
    }
  ],
  graphics: [
    {
      id: "brand-bug",
      name: "Brand bug",
      kind: "bug",
      text: "CoreVideo Pro",
      position: "top-right",
      accent: "#44c1a1",
      enabled: true
    },
    {
      id: "live-banner",
      name: "Live banner",
      kind: "banner",
      text: "Live webinar",
      position: "bottom-right",
      accent: "#ec5757",
      enabled: false
    },
    {
      id: "question-cta",
      name: "Question CTA",
      kind: "cta",
      text: "Submit questions in chat",
      position: "center",
      accent: "#f0a85c",
      enabled: false
    }
  ],
  brandKit: {
    name: "CoreVideo Pro House",
    logoText: "CoreVideo Pro",
    brandColor: "#44c1a1",
    accentColor: "#f0a85c",
    backgroundColor: "#0c1118",
    backgroundImageUrl: "",
    fontFamily: "Inter",
    lowerThirdStyle: "gradient"
  },
  colorGrade: {
    lut: "none",
    exposure: 0,
    contrast: 0,
    saturation: 0,
    temperature: 0
  },
  virtualCamera: {
    enabled: false,
    deviceName: "CoreVideo Pro Camera",
    mirrored: false
  },
  mediaBin: [
    { id: "stinger-1", name: "Brand stinger", kind: "stinger", durationMs: 900 },
    { id: "lower-third-1", name: "Host lower-third", kind: "lower-third" },
    { id: "audio-bed-1", name: "Intro bed", kind: "audio-bed", durationMs: 30000 }
  ],
  videoEffects: initialParticipants.map((participant) => ({
    participantId: participant.id,
    cropMode: "auto",
    manualZoom: 1,
    chromaKeyEnabled: false,
    chromaKeyColor: "green",
    spillSuppression: 42
  })),
  captions: "The active speaker is framed automatically while the slides remain readable.",
  participants: initialParticipants,
  mediaFrames: [],
  scenes: [
    { id: "intro", name: "Intro", type: "intro", layout: "host-focus", automation: "Host lower-third + countdown" },
    { id: "interview", name: "Interview", type: "interview", layout: "two-up", automation: "Two-up speaker hold" },
    {
      id: "speaker-slides",
      name: "Speaker + Slides",
      type: "slides",
      layout: "speaker-slides",
      automation: "Presenter priority + captions",
      selected: true
    },
    { id: "panel", name: "Panel", type: "panel", layout: "smart-grid", automation: "Active speaker plus gallery" },
    { id: "closing", name: "Closing", type: "closing", layout: "outro", automation: "Host outro + CTA" }
  ],
  output: {
    resolution: "1920x1080",
    fps: 60,
    bitrateMbps: 8.2,
    droppedFrames: 0,
    encoderLoad: 42,
    network: "excellent",
    availableDiskGb: 247.3
  },
  captureDevices: []
};
