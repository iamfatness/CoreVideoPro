import type { DirectorIntelligenceSignals, FloorPersonSignal } from "./directorSignals";
import type { AiDirectorProvider, DirectorProposal } from "./directorStrategy";

/**
 * Local, deterministic director. The native and TypeScript decisions use the
 * same floor snapshot: guest score earns person slots, and source aliases are
 * collapsed before a scene can be proposed. No ML or remote inference.
 */

const clampConfidence = (value: number): number => Math.max(0, Math.min(100, Math.round(value)));

function selectFloorProposal(signals: DirectorIntelligenceSignals, floor: FloorPersonSignal[]): DirectorProposal {
  const people = new Map<string, FloorPersonSignal>();
  for (const person of floor) {
    if (!person.hasVideo || !person.id) continue;
    const prior = people.get(person.id);
    people.set(person.id, prior ? {
      ...prior,
      isHost: prior.isHost || person.isHost,
      talkingNow: prior.talkingNow || person.talkingNow,
      score: Math.max(prior.score, person.score),
      lastSpokeAtMs: Math.max(prior.lastSpokeAtMs, person.lastSpokeAtMs),
      sourceId: person.sourceId.startsWith("zoom:") ? person.sourceId : prior.sourceId
    } : person);
  }
  const unique = [...people.values()];
  const guests = unique.filter((person) => !person.isHost)
    .sort((a, b) => b.score - a.score || b.lastSpokeAtMs - a.lastSpokeAtMs || a.id.localeCompare(b.id));
  const hosts = unique.filter((person) => person.isHost)
    .sort((a, b) => b.score - a.score || a.id.localeCompare(b.id));
  const bind = (people: FloorPersonSignal[]) => people.map((person, slotIndex) =>
    ({ slotIndex, personId: person.id, sourceId: person.sourceId }));
  const penalty = Math.min(8, signals.feedHealth.degradedCount * 2);
  if (signals.screenShare.active) {
    return { ruleId: "screen-share-priority", recommendedSceneId: "speaker-slides",
      confidence: clampConfidence(96 + (signals.screenShare.sharerId ? 1 : -2) - penalty),
      rationale: "Share surface leads; guest talker is the picture-in-picture.",
      slotBindings: bind(guests.length ? guests.slice(0, 1) : hosts.slice(0, 1)) };
  }
  if (guests.length === 0) {
    return { ruleId: "single-speaker", recommendedSceneId: "intro", confidence: hosts.length ? 90 : 0,
      rationale: hosts.length ? "Only host is on camera." : "No guest video or host camera is available.",
      slotBindings: bind(hosts.slice(0, 1)) };
  }
  const scoredCount = guests.filter((person) => person.score > 0).length;
  if (scoredCount === 0) {
    return { ruleId: "single-speaker", recommendedSceneId: "intro", confidence: 0,
      rationale: "No guest has earned a talker slot yet.", slotBindings: [] };
  }
  if ((guests.length >= 5 && scoredCount >= 1) || (guests.length >= 3 && scoredCount >= 2)) {
    return { ruleId: "panel-discussion", recommendedSceneId: "panel",
      confidence: clampConfidence(90 + (scoredCount >= 3 ? 3 : 0) - penalty),
      rationale: "Guest floor supports a gallery.", slotBindings: bind(guests.slice(0, 6)) };
  }
  const second = guests[1];
  if (second && guests[0].score > 0 &&
      (second.talkingNow || (second.lastSpokeAtMs > 0 &&
        signals.elapsedMs - second.lastSpokeAtMs <= 12_000))) {
    return { ruleId: "focused-interview", recommendedSceneId: "interview",
      confidence: clampConfidence(92 - (signals.speakerTurns.crossTalk ? 4 : 0) - penalty),
      rationale: "Two distinct guests have the floor within 12 seconds.",
      slotBindings: bind(guests.slice(0, 2)) };
  }
  return { ruleId: "single-speaker", recommendedSceneId: "intro",
    confidence: clampConfidence(90 + signals.engagement.meanAudioLevel * 6 - penalty),
    rationale: "One guest has earned the current talker slot.", slotBindings: bind(guests.slice(0, 1)) };
}

/**
 * Deterministic, pure local scene selection over the richer signal bundle.
 * Exported for direct unit testing; the provider just awaits this.
 */
export function selectLocalProposal(signals: DirectorIntelligenceSignals): DirectorProposal {
  return selectFloorProposal(signals, signals.floor);
}

export class LocalDirectorProvider implements AiDirectorProvider {
  readonly id = "local-director-v1";

  async propose(signals: DirectorIntelligenceSignals): Promise<DirectorProposal> {
    return selectLocalProposal(signals);
  }
}

export const localDirectorProvider = new LocalDirectorProvider();
