/**
 * `FetchLike` (see `../mukanaClient.js`) over Node's global `fetch`. The
 * `signal` is passed straight through to `fetch`, which is what makes an
 * aborted `AbortController` surface as a rejection with `name ===
 * "AbortError"` — the behavior `mukanaPoller`'s hung-request handling
 * depends on. See `nodeFetch.test.ts` for the rule-9 conforming fixture: a
 * real local `node:http` server that never responds, proving the abort
 * path against the actual runtime rather than a mock.
 */

import type { FetchLike, FetchResponse } from "../mukanaClient.js";

export const nodeFetch: FetchLike = async (url, init) => {
  const response = await fetch(url, { signal: init?.signal });
  const mapped: FetchResponse = {
    ok: response.ok,
    status: response.status,
    text: () => response.text()
  };
  return mapped;
};
