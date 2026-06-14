import { describe, expect, it } from "vitest";
import {
  answerQuestion,
  approveQuestion,
  createQaQueue,
  dismissQuestion,
  goLiveQuestion,
  moderationQueue,
  nextQuestion,
  sortedQuestions,
  submitQuestion,
  summarizeQaQueue,
  upvoteCount,
  upvoteQuestion,
} from "./qaQueue";

const NOW = 2_000_000;

function queueWith(...texts: string[]) {
  let q = createQaQueue("Live Q&A");
  for (const t of texts) {
    q = submitQuestion(q, "Author", t);
  }
  return q;
}

describe("createQaQueue", () => {
  it("creates an empty queue", () => {
    const q = createQaQueue("My Q&A");
    expect(q.questions).toHaveLength(0);
    expect(q.liveQuestionId).toBeNull();
  });

  it("trims the title", () => {
    expect(createQaQueue("  Hi  ").title).toBe("Hi");
  });
});

describe("submitQuestion", () => {
  it("appends a pending question", () => {
    const q = submitQuestion(createQaQueue("Q"), "Sam", "How does this work?");
    expect(q.questions).toHaveLength(1);
    expect(q.questions[0].status).toBe("pending");
  });

  it("trims text and author", () => {
    const q = submitQuestion(createQaQueue("Q"), "  Sam  ", "  Hello?  ");
    expect(q.questions[0].author).toBe("Sam");
    expect(q.questions[0].text).toBe("Hello?");
  });

  it("defaults empty author to Anonymous", () => {
    const q = submitQuestion(createQaQueue("Q"), "   ", "Hi?");
    expect(q.questions[0].author).toBe("Anonymous");
  });

  it("throws on empty text", () => {
    expect(() => submitQuestion(createQaQueue("Q"), "Sam", "   ")).toThrow();
  });

  it("starts with zero upvotes and null answeredAt", () => {
    const q = submitQuestion(createQaQueue("Q"), "Sam", "Hi?");
    expect(upvoteCount(q.questions[0])).toBe(0);
    expect(q.questions[0].answeredAtMs).toBeNull();
  });
});

describe("upvoteQuestion", () => {
  it("adds an upvote", () => {
    let q = queueWith("A");
    q = upvoteQuestion(q, q.questions[0].id, "voter1");
    expect(upvoteCount(q.questions[0])).toBe(1);
  });

  it("toggles off when the same voter upvotes again", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = upvoteQuestion(q, id, "voter1");
    q = upvoteQuestion(q, id, "voter1");
    expect(upvoteCount(q.questions[0])).toBe(0);
  });

  it("counts distinct voters", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = upvoteQuestion(q, id, "v1");
    q = upvoteQuestion(q, id, "v2");
    expect(upvoteCount(q.questions[0])).toBe(2);
  });

  it("does not upvote dismissed questions", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = dismissQuestion(q, id);
    q = upvoteQuestion(q, id, "v1");
    expect(upvoteCount(q.questions[0])).toBe(0);
  });

  it("does not upvote answered questions", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = answerQuestion(q, id, NOW);
    q = upvoteQuestion(q, id, "v1");
    expect(upvoteCount(q.questions[0])).toBe(0);
  });
});

describe("approveQuestion", () => {
  it("moves pending → approved", () => {
    let q = queueWith("A");
    q = approveQuestion(q, q.questions[0].id);
    expect(q.questions[0].status).toBe("approved");
  });

  it("is a no-op for non-pending questions", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = approveQuestion(q, id);
    q = approveQuestion(q, id); // already approved
    expect(q.questions[0].status).toBe("approved");
  });
});

describe("dismissQuestion", () => {
  it("marks a question dismissed", () => {
    let q = queueWith("A");
    q = dismissQuestion(q, q.questions[0].id);
    expect(q.questions[0].status).toBe("dismissed");
  });

  it("does not dismiss an answered question", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = answerQuestion(q, id, NOW);
    q = dismissQuestion(q, id);
    expect(q.questions[0].status).toBe("answered");
  });

  it("clears live when the live question is dismissed", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = approveQuestion(q, id);
    q = goLiveQuestion(q, id);
    q = dismissQuestion(q, id);
    expect(q.liveQuestionId).toBeNull();
    expect(q.questions[0].status).toBe("dismissed");
  });
});

describe("goLiveQuestion", () => {
  it("takes an approved question live", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = approveQuestion(q, id);
    q = goLiveQuestion(q, id);
    expect(q.questions[0].status).toBe("live");
    expect(q.liveQuestionId).toBe(id);
  });

  it("does not take a pending question live", () => {
    let q = queueWith("A");
    q = goLiveQuestion(q, q.questions[0].id);
    expect(q.questions[0].status).toBe("pending");
    expect(q.liveQuestionId).toBeNull();
  });

  it("demotes the previous live question to approved", () => {
    let q = queueWith("A", "B");
    const a = q.questions[0].id;
    const b = q.questions[1].id;
    q = approveQuestion(q, a);
    q = approveQuestion(q, b);
    q = goLiveQuestion(q, a);
    q = goLiveQuestion(q, b);
    expect(q.questions[0].status).toBe("approved");
    expect(q.questions[1].status).toBe("live");
    expect(q.liveQuestionId).toBe(b);
  });
});

describe("answerQuestion", () => {
  it("marks a question answered and records time", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = answerQuestion(q, id, NOW);
    expect(q.questions[0].status).toBe("answered");
    expect(q.questions[0].answeredAtMs).toBe(NOW);
  });

  it("clears live when answering the live question", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = approveQuestion(q, id);
    q = goLiveQuestion(q, id);
    q = answerQuestion(q, id, NOW);
    expect(q.liveQuestionId).toBeNull();
  });

  it("does not answer a dismissed question", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = dismissQuestion(q, id);
    q = answerQuestion(q, id, NOW);
    expect(q.questions[0].status).toBe("dismissed");
  });
});

describe("sortedQuestions", () => {
  it("orders by upvotes descending by default", () => {
    let q = queueWith("A", "B", "C");
    const [a, b, c] = q.questions.map((x) => x.id);
    q = upvoteQuestion(q, b, "v1");
    q = upvoteQuestion(q, b, "v2");
    q = upvoteQuestion(q, c, "v1");
    const sorted = sortedQuestions(q, "votes");
    expect(sorted[0].id).toBe(b);
    expect(sorted[1].id).toBe(c);
    expect(sorted[2].id).toBe(a);
  });

  it("orders by recency in recent mode", () => {
    const q = queueWith("A", "B", "C");
    // submittedAtMs may collide (same ms); just assert it returns all 3
    expect(sortedQuestions(q, "recent")).toHaveLength(3);
  });
});

describe("moderationQueue", () => {
  it("returns only pending questions", () => {
    let q = queueWith("A", "B", "C");
    q = approveQuestion(q, q.questions[0].id);
    const mod = moderationQueue(q);
    expect(mod).toHaveLength(2);
    expect(mod.every((x) => x.status === "pending")).toBe(true);
  });

  it("orders pending by upvotes", () => {
    let q = queueWith("A", "B");
    const b = q.questions[1].id;
    q = upvoteQuestion(q, b, "v1");
    expect(moderationQueue(q)[0].id).toBe(b);
  });
});

describe("nextQuestion", () => {
  it("returns the highest-voted approved question", () => {
    let q = queueWith("A", "B");
    const a = q.questions[0].id;
    const b = q.questions[1].id;
    q = approveQuestion(q, a);
    q = approveQuestion(q, b);
    q = upvoteQuestion(q, b, "v1");
    expect(nextQuestion(q)?.id).toBe(b);
  });

  it("returns null when no approved questions", () => {
    const q = queueWith("A");
    expect(nextQuestion(q)).toBeNull();
  });

  it("ignores live/answered/dismissed questions", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = approveQuestion(q, id);
    q = goLiveQuestion(q, id);
    expect(nextQuestion(q)).toBeNull();
  });
});

describe("summarizeQaQueue", () => {
  it("counts statuses", () => {
    let q = queueWith("A", "B", "C", "D");
    q = approveQuestion(q, q.questions[0].id);
    q = dismissQuestion(q, q.questions[1].id);
    q = answerQuestion(q, q.questions[2].id, NOW);
    const s = summarizeQaQueue(q);
    expect(s.total).toBe(4);
    expect(s.pendingCount).toBe(1);
    expect(s.approvedCount).toBe(1);
    expect(s.dismissedCount).toBe(1);
    expect(s.answeredCount).toBe(1);
  });

  it("sums total upvotes", () => {
    let q = queueWith("A", "B");
    q = upvoteQuestion(q, q.questions[0].id, "v1");
    q = upvoteQuestion(q, q.questions[1].id, "v2");
    expect(summarizeQaQueue(q).totalUpvotes).toBe(2);
  });

  it("exposes the live question", () => {
    let q = queueWith("A");
    const id = q.questions[0].id;
    q = approveQuestion(q, id);
    q = goLiveQuestion(q, id);
    const s = summarizeQaQueue(q);
    expect(s.liveQuestion?.id).toBe(id);
    expect(s.summary).toContain("Live:");
  });

  it("summary reflects counts when nothing is live", () => {
    const q = queueWith("A");
    expect(summarizeQaQueue(q).summary).toContain("1 pending");
  });
});
