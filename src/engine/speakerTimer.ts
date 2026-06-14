/**
 * Speaker timer engine.
 * Tracks per-participant cumulative speaking time and enforces equal-time
 * policies for panel discussions, debates, and structured formats.
 */

export type SpeakerRecord = {
  participantId: string;
  name: string;
  /** Total speaking seconds accumulated */
  totalSec: number;
  /** Whether this participant is currently speaking */
  isSpeaking: boolean;
  /** Unix timestamp when current speaking turn started (null if not speaking) */
  turnStartedAt: number | null;
};

export type SpeakerTimerState = {
  speakers: SpeakerRecord[];
  /** Target speaking seconds per speaker (0 = no limit) */
  targetPerSpeakerSec: number;
  totalElapsedSec: number;
};

export type SpeakerAlert = {
  participantId: string;
  name: string;
  severity: "warning" | "over-time";
  message: string;
};

export type SpeakerTimerReport = {
  state: SpeakerTimerState;
  alerts: SpeakerAlert[];
  dominantSpeakerId: string | null;
  balanceScore: number; // 0–100, higher = more equal
  summary: string;
};

/** Warn at 80% of target, error at 100% */
const WARNING_FRACTION = 0.8;

/**
 * Create a fresh speaker timer with a list of participants.
 */
export function createSpeakerTimer(
  participants: Array<{ id: string; name: string }>,
  targetPerSpeakerSec = 0
): SpeakerTimerState {
  return {
    speakers: participants.map((p) => ({
      participantId: p.id,
      name: p.name,
      totalSec: 0,
      isSpeaking: false,
      turnStartedAt: null,
    })),
    targetPerSpeakerSec,
    totalElapsedSec: 0,
  };
}

/**
 * Mark a participant as the current active speaker.
 * Flushes any previous speaker's turn first.
 */
export function setActiveSpeaker(
  state: SpeakerTimerState,
  nowUnixSec: number,
  speakingId: string | null
): SpeakerTimerState {
  const speakers = state.speakers.map((s) => {
    const wasActive = s.isSpeaking;
    const becomesActive = s.participantId === speakingId;

    if (wasActive && !becomesActive && s.turnStartedAt !== null) {
      // Flush completed turn
      return {
        ...s,
        totalSec: s.totalSec + (nowUnixSec - s.turnStartedAt),
        isSpeaking: false,
        turnStartedAt: null,
      };
    }

    if (!wasActive && becomesActive) {
      return { ...s, isSpeaking: true, turnStartedAt: nowUnixSec };
    }

    if (wasActive && becomesActive) {
      // Already speaking — no change
      return s;
    }

    return { ...s, isSpeaking: false, turnStartedAt: null };
  });

  return { ...state, speakers };
}

/**
 * Snapshot totals at nowUnixSec, adding any in-progress turn.
 */
export function snapshotTotals(
  state: SpeakerTimerState,
  nowUnixSec: number
): SpeakerTimerState {
  const speakers = state.speakers.map((s) => {
    if (s.isSpeaking && s.turnStartedAt !== null) {
      return { ...s, totalSec: s.totalSec + (nowUnixSec - s.turnStartedAt), turnStartedAt: nowUnixSec };
    }
    return s;
  });
  return { ...state, speakers };
}

/**
 * Reset all timers to zero.
 */
export function resetSpeakerTimer(state: SpeakerTimerState): SpeakerTimerState {
  return {
    ...state,
    speakers: state.speakers.map((s) => ({
      ...s,
      totalSec: 0,
      isSpeaking: false,
      turnStartedAt: null,
    })),
    totalElapsedSec: 0,
  };
}

/**
 * Add a new participant to an existing timer session.
 */
export function addSpeaker(
  state: SpeakerTimerState,
  participant: { id: string; name: string }
): SpeakerTimerState {
  if (state.speakers.some((s) => s.participantId === participant.id)) {
    return state;
  }
  return {
    ...state,
    speakers: [
      ...state.speakers,
      {
        participantId: participant.id,
        name: participant.name,
        totalSec: 0,
        isSpeaking: false,
        turnStartedAt: null,
      },
    ],
  };
}

/**
 * Compute alerts for speakers who are near or over their time target.
 */
export function computeSpeakerAlerts(state: SpeakerTimerState): SpeakerAlert[] {
  if (state.targetPerSpeakerSec <= 0) return [];

  const alerts: SpeakerAlert[] = [];

  for (const s of state.speakers) {
    const fraction = s.totalSec / state.targetPerSpeakerSec;
    if (fraction >= 1) {
      alerts.push({
        participantId: s.participantId,
        name: s.name,
        severity: "over-time",
        message: `${s.name} is ${formatSec(s.totalSec - state.targetPerSpeakerSec)} over time`,
      });
    } else if (fraction >= WARNING_FRACTION) {
      alerts.push({
        participantId: s.participantId,
        name: s.name,
        severity: "warning",
        message: `${s.name} has ${formatSec(state.targetPerSpeakerSec - s.totalSec)} remaining`,
      });
    }
  }

  return alerts;
}

/**
 * Compute a balance score (0–100) based on speaking time distribution.
 * 100 = perfectly equal; 0 = one person spoke all the time.
 */
export function computeBalanceScore(state: SpeakerTimerState): number {
  const n = state.speakers.length;
  if (n <= 1) return 100;

  const totals = state.speakers.map((s) => s.totalSec);
  const totalTime = totals.reduce((a, b) => a + b, 0);
  if (totalTime === 0) return 100;

  const mean = totalTime / n;
  const maxDeviation = mean * (n - 1); // worst case: one person speaks all
  const actualDeviation = totals.reduce((sum, t) => sum + Math.abs(t - mean), 0) / 2;

  return Math.round(Math.max(0, (1 - actualDeviation / maxDeviation) * 100));
}

/**
 * Identify the speaker with the most cumulative time.
 */
export function dominantSpeaker(state: SpeakerTimerState): SpeakerRecord | null {
  if (state.speakers.length === 0) return null;
  return state.speakers.reduce((best, s) => (s.totalSec > best.totalSec ? s : best), state.speakers[0]);
}

/**
 * Full report combining alerts, balance, and dominant speaker.
 */
export function buildSpeakerReport(state: SpeakerTimerState): SpeakerTimerReport {
  const alerts = computeSpeakerAlerts(state);
  const balanceScore = computeBalanceScore(state);
  const dominant = dominantSpeaker(state);
  const totalTime = state.speakers.reduce((sum, s) => sum + s.totalSec, 0);

  let summary: string;
  if (totalTime === 0) {
    summary = "No speaking time recorded";
  } else if (state.speakers.length === 0) {
    summary = "No speakers";
  } else {
    const topName = dominant?.name ?? "Unknown";
    summary = `${topName} leads — balance ${balanceScore}%`;
  }

  return {
    state,
    alerts,
    dominantSpeakerId: dominant?.participantId ?? null,
    balanceScore,
    summary,
  };
}

export function formatSec(seconds: number): string {
  const abs = Math.abs(Math.round(seconds));
  const m = Math.floor(abs / 60);
  const s = abs % 60;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

export function speakerSharePct(record: SpeakerRecord, allSpeakers: SpeakerRecord[]): number {
  const total = allSpeakers.reduce((sum, s) => sum + s.totalSec, 0);
  if (total === 0) return 0;
  return Math.round((record.totalSec / total) * 100);
}
