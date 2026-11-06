# Mac command response attribution — #1178

Two real show-drill samples exceeded the 100 ms command-response gate: 135.9 ms on a Windows-only workspace PR and 240.4 ms on a Windows-guarded Tiles fixture PR. Neither changed the Mac media implementation. This narrows patch attribution but does not establish runner contention or a product cause. An infrastructure cancellation with zero executed steps is separate from these executed failures.

The existing aggregate p99 loses the step identity, command types and timing boundary. Preserve bounded per-request evidence: relative monotonic start, caller-observed duration, response-reader timing, command type names, timeout outcome and nearby bounded diagnostic lines. Never serialize command arguments or whole snapshots into this report. Reader timing describes when Python received/parses a response, not native execution time. A reader delay cannot by itself prove OS scheduling contention.

The existing 100 ms gate and request population remain unchanged. Generate an evidence artifact on success and failure, including early timeout returns. Hosted CI retains it even when the drill fails. Report any evidence truncation. Preserve the historical failed logs; a later green sample is not a repair or causal explanation.

Validate with a controlled subprocess responding after a deliberate delay, a caller polling delay, and a missing response. The same measured request must still enter the aggregate gate population exactly once; timeouts remain failures. Run one instrumented hosted sample and compare the named slow step with contemporaneous evidence. Reproduce a demonstrated initiating condition before changing product code. If the cause remains unknown, keep the issue and blocked qualification visible rather than increasing the budget or claiming closure.

## Follow-up after the 103 ms sample

The latest hosted sample failed at enable-diagnostics: 102.969 ms total, with 98.548 ms before reader receipt, 1.997 ms parsing and 2.425 ms caller continuation. This does not attribute native execution versus transport/reader scheduling. Existing native request/writer warnings start at 500/200 ms and the report filtered those categories out, so a 103 ms failure can have no useful native phase evidence.

Add opt-in verbose measurements for command queue, assembly-capacity wait, core-lock wait, handler and deferred-engine time; response assembly queue/projection/serialization/enqueue; and response-writer queue/flush. Use the existing bounded asynchronous logger, no command arguments or snapshots, and retain the normal warning thresholds and unchanged 100 ms acceptance gate. Keep a bounded 512-line native timing appendix with omission count and stderr-drain status because asynchronous diagnostics can arrive after the response. Receipt timestamps are not native timestamps or exact request correlation. Missing/dropped evidence stays unknown.

Validate delayed post-response diagnostic delivery, bounded omissions, and unchanged gate population/privacy through the controlled subprocess. Compile and run native RPC regressions after the active recording measurement; use hosted Mac artifacts for causal attribution. A green sample alone still does not close this issue. This diagnostic work remains outside the frozen beta.

## Core-lock wait demonstrated in the 199 ms sample

At `cea2b30d`, run 42284929309 failed with a 199.222 ms enable-diagnostics request. Its sole adjacent request-phase record reports 186.117 ms waiting for the core lock, compared with 2.147 ms handling, 3.586 ms projection and 2.377 ms serialization. The reader/caller contribution is 3.649 ms. A contemporaneous render STALL reports 289 ms rendering. Requests are serialized by this drill, so the first request and first native request record give strong boundary attribution; receipt adjacency alone still cannot identify the exact render tick or GPU operation.

The request starts 1.552 seconds into the process. The later guardrail aggregate reports a 1.417-second worst render hold, but the report did not retain render/lock attribution emitted before the first request. Add those existing fixed-format prefixes to the same bounded appendix, including `lock-hold-attribution`. Preserve all startup requests in the unchanged 100 ms population; do not sleep away the failure or reset counters. A controlled child emitting render phases before its first response must fail with the old filter and pass with the new one. The existing 512-line bound, omissions, asynchronous drain and privacy restrictions remain in force.

The next hosted sample must retain early rendering phases so an initiating operation can be reproduced before changing the renderer. Native logger loss (two records later in this failed run) remains explicit. The existing record-rate and render-over-budget failures also remain failures. This change repairs missing evidence, not command latency; keep the issue and dependent Tiles qualification open.
