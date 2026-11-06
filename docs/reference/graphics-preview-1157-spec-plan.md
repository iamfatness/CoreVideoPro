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
