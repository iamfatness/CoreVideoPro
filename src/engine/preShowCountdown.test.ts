import { describe, expect, it } from "vitest";
import {
  buildPreShowCues,
  computeCountdown,
  formatTMinus,
  markShowLive,
  markShowOver,
} from "./preShowCountdown";

const BASE_SHOW = {
  scheduledAtUnixSec: 10000,
  lobbyDurationSec: 600, // 10 min lobby
  showTitle: "Morning Keynote",
};

describe("computeCountdown — phase transitions", () => {
  it("returns standby before lobby opens", () => {
    const state = computeCountdown(9000, BASE_SHOW); // 1000s before lobby start (9400)
    expect(state.phase).toBe("standby");
    expect(state.secondsToLive).toBe(1000);
  });

  it("returns lobby when inside lobby but outside hot zone", () => {
    // lobbyStartsAt = 10000 - 600 = 9400; hot starts at 9940
    const state = computeCountdown(9500, BASE_SHOW); // 500s to go-live, 440s into lobby
    expect(state.phase).toBe("lobby");
  });

  it("returns hot when within 60s of go-live", () => {
    const state = computeCountdown(9960, BASE_SHOW); // 40s to go-live
    expect(state.phase).toBe("hot");
  });

  it("returns live once past scheduled go-live", () => {
    const state = computeCountdown(10001, BASE_SHOW);
    expect(state.phase).toBe("live");
  });

  it("secondsToLive is negative when overdue", () => {
    const state = computeCountdown(10030, BASE_SHOW);
    expect(state.secondsToLive).toBe(-30);
  });
});

describe("computeCountdown — goLiveNow", () => {
  it("is false outside hot zone", () => {
    const state = computeCountdown(9500, BASE_SHOW);
    expect(state.goLiveNow).toBe(false);
  });

  it("is false in hot zone with >5s remaining", () => {
    const state = computeCountdown(9960, BASE_SHOW); // 40s to go-live
    expect(state.goLiveNow).toBe(false);
  });

  it("is true in hot zone with ≤5s remaining", () => {
    const state = computeCountdown(9995, BASE_SHOW); // 5s to go-live
    expect(state.goLiveNow).toBe(true);
  });
});

describe("computeCountdown — hotWarning", () => {
  it("is null when not in hot phase", () => {
    const state = computeCountdown(9500, BASE_SHOW);
    expect(state.hotWarning).toBeNull();
  });

  it("contains 'stand by' message during hot phase", () => {
    const state = computeCountdown(9960, BASE_SHOW);
    expect(state.hotWarning).toContain("stand by");
  });
});

describe("computeCountdown — tMinusLabel", () => {
  it("formats T-minus correctly before go-live", () => {
    const state = computeCountdown(9700, BASE_SHOW); // 300s = 5min to go-live
    expect(state.tMinusLabel).toBe("T-05:00");
  });

  it("formats T-plus when overdue", () => {
    const state = computeCountdown(10090, BASE_SHOW); // 90s past
    expect(state.tMinusLabel).toBe("T+01:30");
  });
});

describe("computeCountdown — summary", () => {
  it("includes show title in standby summary", () => {
    const state = computeCountdown(9000, BASE_SHOW);
    expect(state.summary).toContain("Morning Keynote");
    expect(state.summary).toContain("standing by");
  });

  it("includes show title in lobby summary", () => {
    const state = computeCountdown(9500, BASE_SHOW);
    expect(state.summary).toContain("lobby open");
  });

  it("shows HOT in hot-phase summary", () => {
    const state = computeCountdown(9960, BASE_SHOW);
    expect(state.summary).toContain("HOT");
  });

  it("shows LIVE in live summary", () => {
    const state = computeCountdown(10010, BASE_SHOW);
    expect(state.summary).toContain("LIVE");
  });
});

describe("markShowLive", () => {
  it("sets phase to live and clears warnings", () => {
    const state = computeCountdown(9960, BASE_SHOW);
    const live = markShowLive(state);
    expect(live.phase).toBe("live");
    expect(live.goLiveNow).toBe(false);
    expect(live.hotWarning).toBeNull();
    expect(live.summary).toBe("Show is live");
  });

  it("preserves tMinusLabel and secondsToLive from original state", () => {
    const state = computeCountdown(9960, BASE_SHOW);
    const live = markShowLive(state);
    expect(live.secondsToLive).toBe(state.secondsToLive);
  });
});

describe("markShowOver", () => {
  it("sets phase to post-show", () => {
    const state = computeCountdown(10010, BASE_SHOW);
    const over = markShowOver(state);
    expect(over.phase).toBe("post-show");
    expect(over.goLiveNow).toBe(false);
    expect(over.hotWarning).toBeNull();
    expect(over.summary).toBe("Show ended");
  });
});

describe("formatTMinus", () => {
  it("formats zero as T-00:00", () => {
    expect(formatTMinus(0)).toBe("T-00:00");
  });

  it("formats minutes and seconds without hours", () => {
    expect(formatTMinus(300)).toBe("T-05:00");
    expect(formatTMinus(90)).toBe("T-01:30");
  });

  it("formats hours when >= 3600s", () => {
    expect(formatTMinus(3661)).toBe("T-1:01:01");
  });

  it("uses + sign when overdue (negative input)", () => {
    expect(formatTMinus(-120)).toBe("T+02:00");
  });

  it("rounds to nearest second", () => {
    expect(formatTMinus(59.6)).toBe("T-01:00");
  });
});

describe("buildPreShowCues", () => {
  it("returns exactly 3 cues", () => {
    const cues = buildPreShowCues(9000, BASE_SHOW);
    expect(cues).toHaveLength(3);
  });

  it("first cue is Lobby opens at correct time", () => {
    const cues = buildPreShowCues(9000, BASE_SHOW);
    expect(cues[0].label).toBe("Lobby opens");
    expect(cues[0].atUnixSec).toBe(9400); // 10000 - 600
  });

  it("second cue is Hot zone at correct time", () => {
    const cues = buildPreShowCues(9000, BASE_SHOW);
    expect(cues[1].label).toBe("Hot zone");
    expect(cues[1].atUnixSec).toBe(9940); // 10000 - 60
  });

  it("third cue is Go live at scheduled time", () => {
    const cues = buildPreShowCues(9000, BASE_SHOW);
    expect(cues[2].label).toBe("Go live");
    expect(cues[2].atUnixSec).toBe(10000);
  });

  it("secondsFromNow is correct relative to nowUnixSec", () => {
    const cues = buildPreShowCues(9000, BASE_SHOW);
    expect(cues[0].secondsFromNow).toBe(400); // 9400 - 9000
    expect(cues[2].secondsFromNow).toBe(1000); // 10000 - 9000
  });

  it("secondsFromNow is negative for past cues", () => {
    // now = 9500, lobby opened at 9400
    const cues = buildPreShowCues(9500, BASE_SHOW);
    expect(cues[0].secondsFromNow).toBe(-100);
  });
});
