/**
 * Poll engine.
 * Manages live audience polls: create, vote, close, tally results,
 * and detect winners for interactive webinar/broadcast formats.
 */

export type PollStatus = "draft" | "open" | "closed" | "results";

export type PollOption = {
  id: string;
  text: string;
  voteCount: number;
};

export type Poll = {
  id: string;
  question: string;
  options: PollOption[];
  status: PollStatus;
  /** Unix timestamp when poll was opened (null = not yet opened) */
  openedAtMs: number | null;
  /** Unix timestamp when poll was closed */
  closedAtMs: number | null;
  /** Allow multiple selections per voter */
  multiSelect: boolean;
  /** Voter IDs who have voted (to prevent duplicates) */
  voterIds: string[];
};

export type PollResults = {
  poll: Poll;
  totalVotes: number;
  winner: PollOption | null;
  isTie: boolean;
  tiedOptions: PollOption[];
  sortedOptions: Array<PollOption & { pct: number }>;
  summary: string;
};

let _pollIdCounter = 0;
function makePollId(): string {
  return `poll-${++_pollIdCounter}-${Math.random().toString(36).slice(2, 5)}`;
}

let _optionIdCounter = 0;
function makeOptionId(): string {
  return `opt-${++_optionIdCounter}`;
}

/**
 * Create a new poll in draft state.
 */
export function createPoll(
  question: string,
  optionTexts: string[],
  multiSelect = false
): Poll {
  if (optionTexts.length < 2) {
    throw new Error("A poll requires at least 2 options");
  }

  return {
    id: makePollId(),
    question: question.trim(),
    options: optionTexts.map((text) => ({
      id: makeOptionId(),
      text: text.trim(),
      voteCount: 0,
    })),
    status: "draft",
    openedAtMs: null,
    closedAtMs: null,
    multiSelect,
    voterIds: [],
  };
}

/**
 * Open a poll for voting.
 */
export function openPoll(poll: Poll, nowMs: number): Poll {
  if (poll.status !== "draft") return poll;
  return { ...poll, status: "open", openedAtMs: nowMs };
}

/**
 * Close a poll for voting and transition to results.
 */
export function closePoll(poll: Poll, nowMs: number): Poll {
  if (poll.status !== "open") return poll;
  return { ...poll, status: "results", closedAtMs: nowMs };
}

/**
 * Reset a poll back to draft, clearing all votes.
 */
export function resetPoll(poll: Poll): Poll {
  return {
    ...poll,
    status: "draft",
    openedAtMs: null,
    closedAtMs: null,
    voterIds: [],
    options: poll.options.map((opt) => ({ ...opt, voteCount: 0 })),
  };
}

/**
 * Cast a vote. Returns updated poll or unchanged poll if voter already voted
 * or poll is not open.
 */
export function castVote(
  poll: Poll,
  voterId: string,
  optionIds: string[]
): { poll: Poll; accepted: boolean; reason: string } {
  if (poll.status !== "open") {
    return { poll, accepted: false, reason: "Poll is not open" };
  }

  if (poll.voterIds.includes(voterId)) {
    return { poll, accepted: false, reason: "Already voted" };
  }

  if (!poll.multiSelect && optionIds.length > 1) {
    return { poll, accepted: false, reason: "Only one selection allowed" };
  }

  const validIds = new Set(poll.options.map((o) => o.id));
  const invalid = optionIds.filter((id) => !validIds.has(id));
  if (invalid.length > 0) {
    return { poll, accepted: false, reason: `Unknown option id(s): ${invalid.join(", ")}` };
  }

  const selectedSet = new Set(optionIds);
  const updated: Poll = {
    ...poll,
    voterIds: [...poll.voterIds, voterId],
    options: poll.options.map((opt) =>
      selectedSet.has(opt.id) ? { ...opt, voteCount: opt.voteCount + 1 } : opt
    ),
  };

  return { poll: updated, accepted: true, reason: "Vote accepted" };
}

/**
 * Add a new option to a draft poll.
 */
export function addPollOption(poll: Poll, text: string): Poll {
  if (poll.status !== "draft") return poll;
  return {
    ...poll,
    options: [...poll.options, { id: makeOptionId(), text: text.trim(), voteCount: 0 }],
  };
}

/**
 * Remove an option from a draft poll (minimum 2 options enforced).
 */
export function removePollOption(poll: Poll, optionId: string): Poll {
  if (poll.status !== "draft" || poll.options.length <= 2) return poll;
  return { ...poll, options: poll.options.filter((o) => o.id !== optionId) };
}

/**
 * Tally results and find the winner(s).
 */
export function tallyResults(poll: Poll): PollResults {
  const totalVotes = poll.options.reduce((sum, o) => sum + o.voteCount, 0);

  const sorted = [...poll.options]
    .sort((a, b) => b.voteCount - a.voteCount)
    .map((o) => ({
      ...o,
      pct: totalVotes > 0 ? Math.round((o.voteCount / totalVotes) * 100) : 0,
    }));

  const topVotes = sorted[0]?.voteCount ?? 0;
  const leaders = sorted.filter((o) => o.voteCount === topVotes);
  const isTie = leaders.length > 1 && topVotes > 0;
  const winner = !isTie && topVotes > 0 ? sorted[0] : null;
  const tiedOptions = isTie ? leaders : [];

  let summary: string;
  if (totalVotes === 0) {
    summary = "No votes yet";
  } else if (isTie) {
    summary = `Tie: ${tiedOptions.map((o) => o.text).join(" vs ")} (${topVotes} votes each)`;
  } else if (winner) {
    summary = `"${winner.text}" leads with ${winner.pct}% (${winner.voteCount}/${totalVotes} votes)`;
  } else {
    summary = `${totalVotes} vote${totalVotes !== 1 ? "s" : ""} counted`;
  }

  return { poll, totalVotes, winner, isTie, tiedOptions, sortedOptions: sorted, summary };
}

/**
 * Duration the poll has been open in seconds.
 */
export function pollOpenDurationSec(poll: Poll, nowMs: number): number {
  if (poll.openedAtMs === null) return 0;
  const endMs = poll.closedAtMs ?? nowMs;
  return Math.round((endMs - poll.openedAtMs) / 1000);
}
