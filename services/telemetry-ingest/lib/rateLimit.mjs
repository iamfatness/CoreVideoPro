/**
 * Per-isolate, in-memory token bucket keyed by client IP.
 *
 * Honest limits (deliberate for beta — documented, not hidden):
 * - State lives in the isolate. Every isolate (and every colo) has its own
 *   buckets, and an isolate recycle resets them, so this bounds abuse
 *   per-isolate, NOT globally. A distributed attacker spreads across colos
 *   and gets a fresh budget in each.
 * - It IS enough to stop accidental client loops (the realistic beta
 *   failure mode: a crash-report retry loop) and casual abuse.
 * - Upgrade path when it matters: Cloudflare's native rate limiting binding
 *   or a Durable Object counter.
 */
export function createIpRateLimiter({
  limit = 60,
  windowMs = 60_000,
  maxEntries = 10_000,
  now = Date.now
} = {}) {
  const buckets = new Map();

  return {
    /** Returns true when the request is allowed, false when rate-limited. */
    allow(ip) {
      const at = now();
      let bucket = buckets.get(ip);
      if (!bucket) {
        if (buckets.size >= maxEntries) {
          prune(buckets, at, windowMs);
          if (buckets.size >= maxEntries) {
            // Last resort: bounded memory wins over strictness (fail open).
            buckets.clear();
          }
        }
        bucket = { tokens: limit, refilledAt: at };
        buckets.set(ip, bucket);
      }

      const refill = ((at - bucket.refilledAt) / windowMs) * limit;
      bucket.tokens = Math.min(limit, bucket.tokens + refill);
      bucket.refilledAt = at;

      if (bucket.tokens < 1) {
        return false;
      }
      bucket.tokens -= 1;
      return true;
    }
  };
}

function prune(buckets, at, windowMs) {
  for (const [ip, bucket] of buckets) {
    if (at - bucket.refilledAt > windowMs) {
      buckets.delete(ip);
    }
  }
}
