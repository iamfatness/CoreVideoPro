# Private Graphics preview: specification and investigation plan

Issue: [#1157](https://github.com/iamfatness/CoreVideoPro/issues/1157). Order/limit: [BACKLOG](../BACKLOG.md).

## Contract and observed gap

A valid `set-grade-preview` request with an explicit JSON null `appearance` is eligible for private preview under the existing native command readiness rules. Omitting that field remains invalid. Request identity/revision and source rules remain enforced. A generated private preview must produce changing pixels for changed text/revision without creating a live Graphics instance or changing Program.

The full-run fixture failed its acceptance assertion before capturing the RPC reply; exact rerun passed. Unknown reply/error means this is not yet a proven parser, rendering or admission bug. Evidence: `D:/CoreVideo-QA/tmp/vcam-timing-1141/merged-full-native.log` and `graphics-preview-exact.log`.

## Method

1. Retain the exact serialized request, full response, command/core readiness state and revision on failure. Do not log operator/provider secrets; the fixture uses synthetic text only.
2. Read `GraphicsRpcPixel.PrivateGraphicsPreviewRequiresExplicitNullAppearanceAndProducesChangingPixels`, RPC handling and preview scheduling; distinguish protocol invalidity from expected transient busy/retry semantics. Inspect preceding full-suite state and owned process cleanup.
3. Exercise missing field, explicit null, valid custom appearance, stale revision, and invalid source. Use a bounded completion observation for async textures. An admission rejection must be classified before deciding whether a test should retry or production should change.
4. Make the minimum causally supported fix. Do not weaken explicit-null validation or change a truthful failure to unconditional success. If transient busy is part of the existing contract, prove that readiness is reached and the exact valid request then produces pixels; do not silently retry all errors.
5. Run the exact red/green fixture and the native Graphics RPC/pixel group, then the integrated suite and compiled Graphics probes. Retain the original failure even if it cannot be reproduced.

## Acceptance

Invalid requests still fail for the correct reason. Valid requests either have a documented actionable refusal or a verified private texture; changing text/revision changes actual pixel content. Program and native on-air instances remain unchanged. A mutation omitting null handling, publishing on Program, or returning an unchanged texture must fail. If investigation only improves failure diagnostics, report that accurately and leave the defect open.

## November 5 investigation and implementation

Built-in definitions are loaded synchronously, ruling out the initial template
startup hypothesis. Two actual admission problems were found instead:

- MediaCore prepared a graphics draft before the controller validated the
  envelope. A deliberately missing `appearance` could start the compiler despite
  the request being rejected, leaving its valid successor competing with that
  work. A preparation-call counter fails deterministically with eager ordering.
- The compiler's nonblocking admission could refuse a valid request on mutex
  contention, but that was reported as a protocol error. A gated admission test
  now distinguishes contention from malformed input without depending on timing.

Preparation is now a lazy callback owned by the preview controller. Identity,
appearance, scopes, capacity and revision checks run before preparation; a lease
refresh keeps its accepted draft without recompiling, and a refused replacement
keeps the previously accepted preview. Compiler contention propagates as the
existing `grade-preview-overloaded` response with retry guidance. No native
blocking/retry loop was added. The pixel fixture retries only that explicit
response, retaining its exact request/revision; protocol errors still fail
immediately with the full synthetic reply printed.

Production Release checks passed: 11 controller tests, two compiler tests, two
RPC/pixel tests (including changed-text shared textures and unchanged on-air
instances), and four managed coordinator tests. The managed check proves the
existing shell resends the same refused revision and accepts its later texture.
Suppressing the busy classification made the gated test fail; restoration passed.
Logs/TRX are under `D:/CoreVideo-QA/overnight-20261105/1157-*` and `test-results`.
The original full-run reply was not recorded, so its exact error cannot be
retrospectively identified. These are verified fixes to the reproduced admission
paths; full integrated qualification remains pending.
