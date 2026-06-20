/**
 * Claude client for the AI-director intelligence service.
 *
 * Calls the Anthropic Messages API via raw HTTP (the idiomatic approach inside a
 * Cloudflare Worker, matching the other `services/` workers). The model id and
 * SDK pattern follow the `claude-api` skill: default to the latest Opus
 * (`claude-opus-4-8`), adaptive thinking, and structured outputs
 * (`output_config.format`) so the response is guaranteed-parseable JSON.
 *
 * The API key is read from `env.ANTHROPIC_API_KEY` (a Worker secret), NEVER
 * committed. With no key configured the caller treats the service as absent and
 * the renderer falls back to the always-on heuristic director.
 */

export const DEFAULT_CLAUDE_MODEL_ID = "claude-opus-4-8";
const ANTHROPIC_MESSAGES_URL = "https://api.anthropic.com/v1/messages";
const ANTHROPIC_VERSION = "2023-06-01";

/** JSON schema constraining the model to a valid director proposal. */
const PROPOSAL_SCHEMA = {
  type: "object",
  properties: {
    ruleId: {
      type: "string",
      enum: ["screen-share-priority", "single-speaker", "focused-interview", "panel-discussion"]
    },
    recommendedSceneId: { type: "string" },
    confidence: { type: "integer" },
    rationale: { type: "string" }
  },
  required: ["ruleId", "recommendedSceneId", "confidence", "rationale"],
  additionalProperties: false
};

const SYSTEM_PROMPT = [
  "You are the AI director for a live multi-camera production switcher.",
  "Given the current Zoom meeting signals you propose ONE scene to cut to.",
  "You only propose; a deterministic safety layer downstream decides whether to take, queue, or hold your proposal, so favor the clearest read of the room.",
  "Rules:",
  "- recommendedSceneId MUST be one of the provided sceneIds.",
  "- ruleId must match your reasoning: screen-share-priority when a screen share should drive the program; single-speaker for a lone dominant presenter; focused-interview for two engaged speakers; panel-discussion for an active multi-speaker room.",
  "- confidence is 0-100: high when the signal is unambiguous, lower near boundaries or during cross-talk / degraded feeds.",
  "- Keep rationale to one short sentence."
].join("\n");

/**
 * Ask Claude for a director proposal. Returns a parsed proposal object (not yet
 * sanitized — the renderer sanitizes again) or throws on any failure so the
 * worker can return a 502 and the renderer can fall back to the heuristic.
 *
 * @param {object} input - the proposal request body (sceneIds, snapshot, signals).
 * @param {{ ANTHROPIC_API_KEY?: string, CLAUDE_MODEL_ID?: string }} env
 * @param {typeof fetch} [fetchImpl]
 */
export async function requestDirectorProposal(input, env, fetchImpl = fetch) {
  const apiKey = env.ANTHROPIC_API_KEY?.trim();
  if (!apiKey) {
    throw new Error("ANTHROPIC_API_KEY is not configured");
  }
  const model = env.CLAUDE_MODEL_ID?.trim() || DEFAULT_CLAUDE_MODEL_ID;

  const userContent = [
    "Propose the next scene for this meeting.",
    "",
    "Valid scene ids: " + JSON.stringify(input.sceneIds ?? []),
    "Current program scene id: " + JSON.stringify(input.activeSceneId ?? null),
    "Director mode: " + JSON.stringify(input.mode ?? "manual"),
    "Snapshot: " + JSON.stringify(input.snapshot ?? {}),
    "Signals: " + JSON.stringify(input.signals ?? {})
  ].join("\n");

  const response = await fetchImpl(ANTHROPIC_MESSAGES_URL, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      "x-api-key": apiKey,
      "anthropic-version": ANTHROPIC_VERSION
    },
    body: JSON.stringify({
      model,
      max_tokens: 1024,
      thinking: { type: "adaptive" },
      system: SYSTEM_PROMPT,
      output_config: {
        format: { type: "json_schema", schema: PROPOSAL_SCHEMA }
      },
      messages: [{ role: "user", content: userContent }]
    })
  });

  if (!response.ok) {
    const detail = await safeText(response);
    throw new Error(`Anthropic API ${response.status}: ${detail}`);
  }

  const payload = await response.json();

  // Safety classifiers can decline; surface as a failure so the renderer falls
  // back to the heuristic rather than acting on an empty response.
  if (payload?.stop_reason === "refusal") {
    throw new Error("Anthropic API refused the request");
  }

  const text = firstTextBlock(payload);
  if (!text) {
    throw new Error("Anthropic API returned no text content");
  }

  const proposal = JSON.parse(text);
  return { proposal, modelId: model };
}

function firstTextBlock(payload) {
  const blocks = Array.isArray(payload?.content) ? payload.content : [];
  for (const block of blocks) {
    if (block?.type === "text" && typeof block.text === "string") {
      return block.text;
    }
  }
  return undefined;
}

async function safeText(response) {
  try {
    return (await response.text()).slice(0, 500);
  } catch {
    return "<unreadable>";
  }
}
