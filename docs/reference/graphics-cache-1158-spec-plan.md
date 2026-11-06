# Graphics cache lease release: specification and investigation plan

Issue: [#1158](https://github.com/iamfatness/CoreVideoPro/issues/1158). Order/limit: [BACKLOG](../BACKLOG.md).

## Contract

`PreparationCache` admission is nonblocking and may refuse during lock contention. A refused request is not an admitted job. Live external leases remain charged even if the cache retires its lookup entry. Total resident plus reserved work never exceeds its budget; releasing the last lease eventually makes resources available to subsequent explicit demand. The render caller never executes or waits for the producer.

The failing fixture has budget=80, job reservations=60 and actual assets=40. Asset 1 is held externally; asset 2 must not be prepared while its reservation plus the live asset exceed 80. After the last external lease is released, a newly admitted attempt for asset 2 must eventually return value 20, with one resident asset of 40 and high-water usage no greater than 80. A successful test must establish the actual admission/completion ordering, not infer it from a transient zero pending count.

## Method

1. Preserve `D:/CoreVideo-QA/tmp/gfx-twitch-merge-1138/full-native.log` and the passing exact comparison. Inspect `GraphicsPreparationCacheTest.cpp` and `graphics/PreparationCache.h`: lookup snapshots, `queued_`, pending byte reservations, producer completion, retirement and external references.
2. Capture admitted/completed/refused/failed counts, residency/reservations and whether each demand was admitted. Test whether an old completion or transient producer-owned reference can satisfy an incorrect fixture precondition. Do not assume this and #1157 share a cause.
3. Use promises/barriers to control the relevant interleaving. Verify fixed expected values (10 then 20), producer calls (one while held, two after release), budget 80 and charged lease 40. Add terminal state/reason to failed assertions.
4. Repair the smallest demonstrated contract violation: fixture ordering if it misobserves nonblocking admission, cache ownership/admission if runtime behavior is wrong. Keep overload/retry policy explicit and avoid busy retries in production.
5. Demonstrate a red counterexample, then exact/cache-suite green results and integrated full-suite evidence. Restore the old faulty ordering as a mutation when practical; it must fail the new check.

## Completion

Both held and released phases are exercised, no producer call occurs on the caller, no unsafe eviction frees a live lease, and demand after proven release completes within a deadlock backstop without assuming host speed. No budget increase or assertion removal. If no reproducible cause is found within the allocation, retain diagnostic evidence and leave the issue open.
