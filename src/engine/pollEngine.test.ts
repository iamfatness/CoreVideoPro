import { describe, expect, it } from "vitest";
import {
  addPollOption,
  castVote,
  closePoll,
  createPoll,
  openPoll,
  pollOpenDurationSec,
  removePollOption,
  resetPoll,
  tallyResults,
} from "./pollEngine";

const NOW = 1_000_000;

function buildOpenPoll() {
  const poll = createPoll("Favourite colour?", ["Red", "Blue", "Green"]);
  return openPoll(poll, NOW);
}

describe("createPoll", () => {
  it("creates a poll in draft state", () => {
    const poll = createPoll("Question?", ["A", "B"]);
    expect(poll.status).toBe("draft");
    expect(poll.options).toHaveLength(2);
    expect(poll.voterIds).toHaveLength(0);
  });

  it("throws when fewer than 2 options", () => {
    expect(() => createPoll("Q?", ["Only"])).toThrow();
  });

  it("trims question and option text", () => {
    const poll = createPoll("  Q  ", ["  A  ", "  B  "]);
    expect(poll.question).toBe("Q");
    expect(poll.options[0].text).toBe("A");
  });

  it("defaults multiSelect to false", () => {
    const poll = createPoll("Q?", ["A", "B"]);
    expect(poll.multiSelect).toBe(false);
  });

  it("accepts multiSelect=true", () => {
    const poll = createPoll("Q?", ["A", "B"], true);
    expect(poll.multiSelect).toBe(true);
  });
});

describe("openPoll / closePoll", () => {
  it("transitions draft → open", () => {
    const poll = createPoll("Q?", ["A", "B"]);
    expect(openPoll(poll, NOW).status).toBe("open");
  });

  it("sets openedAtMs when opened", () => {
    const poll = createPoll("Q?", ["A", "B"]);
    expect(openPoll(poll, NOW).openedAtMs).toBe(NOW);
  });

  it("does not re-open an already open poll", () => {
    const poll = openPoll(createPoll("Q?", ["A", "B"]), NOW);
    expect(openPoll(poll, NOW + 1000).openedAtMs).toBe(NOW); // unchanged
  });

  it("transitions open → results on close", () => {
    const poll = openPoll(createPoll("Q?", ["A", "B"]), NOW);
    expect(closePoll(poll, NOW + 60000).status).toBe("results");
  });

  it("sets closedAtMs when closed", () => {
    const poll = openPoll(createPoll("Q?", ["A", "B"]), NOW);
    expect(closePoll(poll, NOW + 60000).closedAtMs).toBe(NOW + 60000);
  });

  it("does not close a draft poll", () => {
    const poll = createPoll("Q?", ["A", "B"]);
    expect(closePoll(poll, NOW).status).toBe("draft");
  });
});

describe("resetPoll", () => {
  it("resets vote counts to 0", () => {
    let poll = buildOpenPoll();
    const optId = poll.options[0].id;
    ({ poll } = castVote(poll, "v1", [optId]));
    poll = resetPoll(poll);
    expect(poll.options.every((o) => o.voteCount === 0)).toBe(true);
  });

  it("clears voterIds", () => {
    let poll = buildOpenPoll();
    const optId = poll.options[0].id;
    ({ poll } = castVote(poll, "v1", [optId]));
    poll = resetPoll(poll);
    expect(poll.voterIds).toHaveLength(0);
  });

  it("resets status to draft", () => {
    let poll = buildOpenPoll();
    poll = closePoll(poll, NOW + 1000);
    poll = resetPoll(poll);
    expect(poll.status).toBe("draft");
  });
});

describe("castVote", () => {
  it("accepts a valid vote", () => {
    const poll = buildOpenPoll();
    const optId = poll.options[0].id;
    const { accepted, poll: updated } = castVote(poll, "voter1", [optId]);
    expect(accepted).toBe(true);
    expect(updated.options[0].voteCount).toBe(1);
  });

  it("rejects duplicate voter", () => {
    let poll = buildOpenPoll();
    const optId = poll.options[0].id;
    ({ poll } = castVote(poll, "voter1", [optId]));
    const { accepted, reason } = castVote(poll, "voter1", [optId]);
    expect(accepted).toBe(false);
    expect(reason).toContain("Already voted");
  });

  it("rejects vote on closed poll", () => {
    const open = buildOpenPoll();
    const closed = closePoll(open, NOW + 1000);
    const { accepted } = castVote(closed, "v1", [open.options[0].id]);
    expect(accepted).toBe(false);
  });

  it("rejects multi-select when multiSelect=false", () => {
    const poll = buildOpenPoll();
    const { accepted, reason } = castVote(poll, "v1", [poll.options[0].id, poll.options[1].id]);
    expect(accepted).toBe(false);
    expect(reason).toContain("one selection");
  });

  it("accepts multi-select when multiSelect=true", () => {
    const poll = openPoll(createPoll("Q?", ["A", "B", "C"], true), NOW);
    const { accepted } = castVote(poll, "v1", [poll.options[0].id, poll.options[1].id]);
    expect(accepted).toBe(true);
  });

  it("rejects unknown option id", () => {
    const poll = buildOpenPoll();
    const { accepted } = castVote(poll, "v1", ["bad-id"]);
    expect(accepted).toBe(false);
  });

  it("records voterId after successful vote", () => {
    const poll = buildOpenPoll();
    const { poll: updated } = castVote(poll, "voter99", [poll.options[0].id]);
    expect(updated.voterIds).toContain("voter99");
  });
});

describe("addPollOption / removePollOption", () => {
  it("adds option to draft poll", () => {
    const poll = createPoll("Q?", ["A", "B"]);
    const updated = addPollOption(poll, "C");
    expect(updated.options).toHaveLength(3);
    expect(updated.options[2].text).toBe("C");
  });

  it("does not add option to open poll", () => {
    const poll = openPoll(createPoll("Q?", ["A", "B"]), NOW);
    expect(addPollOption(poll, "C").options).toHaveLength(2);
  });

  it("removes option from draft poll", () => {
    const poll = createPoll("Q?", ["A", "B", "C"]);
    const optId = poll.options[2].id;
    expect(removePollOption(poll, optId).options).toHaveLength(2);
  });

  it("does not remove below 2 options", () => {
    const poll = createPoll("Q?", ["A", "B"]);
    const optId = poll.options[0].id;
    expect(removePollOption(poll, optId).options).toHaveLength(2);
  });
});

describe("tallyResults", () => {
  it("returns 0 total votes for fresh poll", () => {
    expect(tallyResults(buildOpenPoll()).totalVotes).toBe(0);
  });

  it("identifies winner correctly", () => {
    let poll = buildOpenPoll();
    const redId = poll.options[0].id;
    const blueId = poll.options[1].id;
    ({ poll } = castVote(poll, "v1", [redId]));
    ({ poll } = castVote(poll, "v2", [redId]));
    ({ poll } = castVote(poll, "v3", [blueId]));
    const results = tallyResults(poll);
    expect(results.winner?.text).toBe("Red");
    expect(results.isTie).toBe(false);
  });

  it("detects a tie", () => {
    let poll = buildOpenPoll();
    const redId = poll.options[0].id;
    const blueId = poll.options[1].id;
    ({ poll } = castVote(poll, "v1", [redId]));
    ({ poll } = castVote(poll, "v2", [blueId]));
    const results = tallyResults(poll);
    expect(results.isTie).toBe(true);
    expect(results.winner).toBeNull();
    expect(results.tiedOptions).toHaveLength(2);
  });

  it("sorts options by descending vote count", () => {
    let poll = buildOpenPoll();
    const greenId = poll.options[2].id;
    ({ poll } = castVote(poll, "v1", [greenId]));
    const results = tallyResults(poll);
    expect(results.sortedOptions[0].id).toBe(greenId);
  });

  it("computes percentage correctly", () => {
    let poll = buildOpenPoll();
    const redId = poll.options[0].id;
    ({ poll } = castVote(poll, "v1", [redId]));
    ({ poll } = castVote(poll, "v2", [redId]));
    ({ poll } = castVote(poll, "v3", [poll.options[1].id]));
    const results = tallyResults(poll);
    // Red: 2/3 = 67%
    expect(results.sortedOptions[0].pct).toBe(67);
  });

  it("summary contains winner text", () => {
    let poll = buildOpenPoll();
    ({ poll } = castVote(poll, "v1", [poll.options[0].id]));
    const results = tallyResults(poll);
    expect(results.summary).toContain("Red");
  });

  it("summary says 'No votes yet' for empty poll", () => {
    expect(tallyResults(buildOpenPoll()).summary).toBe("No votes yet");
  });
});

describe("pollOpenDurationSec", () => {
  it("returns 0 for unopened poll", () => {
    const poll = createPoll("Q?", ["A", "B"]);
    expect(pollOpenDurationSec(poll, NOW)).toBe(0);
  });

  it("returns elapsed seconds while poll is open", () => {
    const poll = openPoll(createPoll("Q?", ["A", "B"]), NOW);
    expect(pollOpenDurationSec(poll, NOW + 30000)).toBe(30);
  });

  it("returns fixed duration after poll closes", () => {
    const poll = closePoll(openPoll(createPoll("Q?", ["A", "B"]), NOW), NOW + 60000);
    expect(pollOpenDurationSec(poll, NOW + 120000)).toBe(60); // uses closedAtMs
  });
});
