/**
 * Audience Q&A queue engine.
 * Moderates live audience questions: submit, upvote, approve/dismiss,
 * push one question "live" on screen, and mark answered.
 * Pure TypeScript — no React, no side effects.
 */

export type QuestionStatus = "pending" | "approved" | "live" | "answered" | "dismissed";

export type Question = {
  id: string;
  author: string;
  text: string;
  submittedAtMs: number;
  upvoterIds: string[];
  status: QuestionStatus;
  answeredAtMs: number | null;
};

export type QaQueue = {
  id: string;
  title: string;
  questions: Question[];
  liveQuestionId: string | null;
};

export type QaQueueSummary = {
  total: number;
  pendingCount: number;
  approvedCount: number;
  answeredCount: number;
  dismissedCount: number;
  totalUpvotes: number;
  liveQuestion: Question | null;
  summary: string;
};

export type QaSortMode = "votes" | "recent";

let _idCounter = 0;
function makeId(prefix = "q"): string {
  _idCounter += 1;
  return `${prefix}-${Date.now()}-${_idCounter}`;
}

/** Number of upvotes a question has. */
export function upvoteCount(question: Question): number {
  return question.upvoterIds.length;
}

/**
 * Create an empty Q&A queue.
 */
export function createQaQueue(title: string): QaQueue {
  return {
    id: makeId("qa"),
    title: title.trim(),
    questions: [],
    liveQuestionId: null,
  };
}

/**
 * Submit a new audience question. Trims input; throws on empty text/author.
 */
export function submitQuestion(queue: QaQueue, author: string, text: string): QaQueue {
  const trimmedText = text.trim();
  const trimmedAuthor = author.trim();
  if (trimmedText.length === 0) {
    throw new Error("Question text cannot be empty");
  }
  const question: Question = {
    id: makeId(),
    author: trimmedAuthor.length > 0 ? trimmedAuthor : "Anonymous",
    text: trimmedText,
    submittedAtMs: Date.now(),
    upvoterIds: [],
    status: "pending",
    answeredAtMs: null,
  };
  return { ...queue, questions: [...queue.questions, question] };
}

/**
 * Toggle an upvote for a voter. A voter may upvote a question at most once;
 * upvoting again removes their vote. Voters cannot upvote dismissed/answered
 * questions.
 */
export function upvoteQuestion(queue: QaQueue, questionId: string, voterId: string): QaQueue {
  return {
    ...queue,
    questions: queue.questions.map((q) => {
      if (q.id !== questionId) return q;
      if (q.status === "dismissed" || q.status === "answered") return q;
      const has = q.upvoterIds.includes(voterId);
      return {
        ...q,
        upvoterIds: has
          ? q.upvoterIds.filter((id) => id !== voterId)
          : [...q.upvoterIds, voterId],
      };
    }),
  };
}

/**
 * Approve a pending question (moderator action). Only pending → approved.
 */
export function approveQuestion(queue: QaQueue, questionId: string): QaQueue {
  return {
    ...queue,
    questions: queue.questions.map((q) =>
      q.id === questionId && q.status === "pending"
        ? { ...q, status: "approved" as QuestionStatus }
        : q
    ),
  };
}

/**
 * Dismiss a question. Allowed unless already answered. Clears live if needed.
 */
export function dismissQuestion(queue: QaQueue, questionId: string): QaQueue {
  const questions = queue.questions.map((q) =>
    q.id === questionId && q.status !== "answered"
      ? { ...q, status: "dismissed" as QuestionStatus }
      : q
  );
  const target = queue.questions.find((q) => q.id === questionId);
  const cleared = target && target.status !== "answered" && queue.liveQuestionId === questionId;
  return { ...queue, questions, liveQuestionId: cleared ? null : queue.liveQuestionId };
}

/**
 * Push a question live on screen. Auto-demotes the previous live question back
 * to approved. Only approved/live questions may go live.
 */
export function goLiveQuestion(queue: QaQueue, questionId: string): QaQueue {
  const target = queue.questions.find((q) => q.id === questionId);
  if (!target || (target.status !== "approved" && target.status !== "live")) {
    return queue;
  }
  const questions = queue.questions.map((q) => {
    if (q.id === queue.liveQuestionId && q.id !== questionId && q.status === "live") {
      return { ...q, status: "approved" as QuestionStatus };
    }
    if (q.id === questionId) {
      return { ...q, status: "live" as QuestionStatus };
    }
    return q;
  });
  return { ...queue, questions, liveQuestionId: questionId };
}

/**
 * Mark a question answered. Records answeredAtMs and clears live if it was live.
 */
export function answerQuestion(queue: QaQueue, questionId: string, nowMs: number): QaQueue {
  const questions = queue.questions.map((q) =>
    q.id === questionId && q.status !== "dismissed"
      ? { ...q, status: "answered" as QuestionStatus, answeredAtMs: nowMs }
      : q
  );
  const cleared = queue.liveQuestionId === questionId;
  return { ...queue, questions, liveQuestionId: cleared ? null : queue.liveQuestionId };
}

/**
 * Return questions sorted for display. "votes" orders by upvotes desc then
 * recency; "recent" orders by submission time desc.
 */
export function sortedQuestions(queue: QaQueue, mode: QaSortMode = "votes"): Question[] {
  const copy = [...queue.questions];
  if (mode === "recent") {
    return copy.sort((a, b) => b.submittedAtMs - a.submittedAtMs);
  }
  return copy.sort((a, b) => {
    const diff = upvoteCount(b) - upvoteCount(a);
    return diff !== 0 ? diff : b.submittedAtMs - a.submittedAtMs;
  });
}

/**
 * Pending questions awaiting moderation, highest-voted first.
 */
export function moderationQueue(queue: QaQueue): Question[] {
  return sortedQuestions(queue, "votes").filter((q) => q.status === "pending");
}

/**
 * The next best approved question to take live (highest upvotes), or null.
 */
export function nextQuestion(queue: QaQueue): Question | null {
  const approved = sortedQuestions(queue, "votes").filter((q) => q.status === "approved");
  return approved[0] ?? null;
}

/**
 * Summarize queue state for dashboard display.
 */
export function summarizeQaQueue(queue: QaQueue): QaQueueSummary {
  let pendingCount = 0;
  let approvedCount = 0;
  let answeredCount = 0;
  let dismissedCount = 0;
  let totalUpvotes = 0;

  for (const q of queue.questions) {
    totalUpvotes += upvoteCount(q);
    switch (q.status) {
      case "pending": pendingCount++; break;
      case "approved": approvedCount++; break;
      case "answered": answeredCount++; break;
      case "dismissed": dismissedCount++; break;
      case "live": break;
    }
  }

  const liveQuestion = queue.questions.find((q) => q.id === queue.liveQuestionId) ?? null;

  const summary = liveQuestion
    ? `Live: "${truncate(liveQuestion.text, 40)}" — ${pendingCount} pending, ${approvedCount} ready`
    : `${pendingCount} pending, ${approvedCount} ready, ${answeredCount} answered`;

  return {
    total: queue.questions.length,
    pendingCount,
    approvedCount,
    answeredCount,
    dismissedCount,
    totalUpvotes,
    liveQuestion,
    summary,
  };
}

function truncate(text: string, max: number): string {
  return text.length <= max ? text : `${text.slice(0, max - 1)}…`;
}
