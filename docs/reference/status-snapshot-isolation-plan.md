# Status snapshot isolation implementation plan

Issue: [#870](https://github.com/iamfatness/CoreVideoPro/issues/870).
Contract: [specification](status-snapshot-isolation-spec.md).
This sequence describes one repair, not a separate ranked backlog. Issue status
and owner order remain in [BACKLOG](../BACKLOG.md).

## 1. Establish the dependency and measure its trigger

Audit `sessionState`, health/profile, encoder/sender and audio projections, all
RPC entry points and their lock ordering. Identify each field's real owner,
existing cached publication, capacity and asynchronous completion semantics.
Keep command intent separate from worker-observed facts.

Add bounded, opt-in timings with separate nested lock waits and body intervals.
Capture a matched failing interval in the original eight-feed/capture Tiles
workload. The 48.8528 ms whole-audio-worker measurement is not a DSP measurement.
Use a deterministic audio-owner stall to establish the current dependency and
an independent pixel judge to detect the resulting Program interruption.

Deliverable: an ownership/field map, retained failing baseline and an attributed
slow interval or clearly outstanding attribution gate. The confirmed dependency
can be repaired while that gate is investigated; issue closure cannot bypass it.

## 2. Implement bounded owner observations with their first consumers

Introduce focused immutable observation/store types. The audio worker captures
its meters and audio-owned facts, releases its work lock, then publishes a small
reference. Reuse encoder/sender owners' cached lifecycle/evidence publications.
Define generation/revision, timestamp, freshness and exhaustion behavior before
wire projections. Verify no store retains PCM, pixels, devices or GPU resources.

Wire audio/encoder/sender status readers to those stores in the same patch.
Exercise startup, idle, failure, shutdown, restart, delayed publication and slow
readers. Maintain current supported source/bus capacity and test the proposed
metadata budgets. Do not ship a new type without its real consumer.

## 3. Separate command capture from response assembly

Extract bounded core-state capture and off-lock assembly. Release `coreMutex`
before nested module reads, JSON construction and response work. Acquire one
immutable owner lease per response and label its generation/revision/time.
Audit cold-start, handshake and explicit health calls for remaining live-lock
fallbacks. Preserve existing synchronous native test behavior through explicit
single-owner observation updates, without unsafe unlocked mutable reads.

Validate that a command is acknowledged when applied and that delayed observations
cannot restore old settings or claim output completion. Keep recording's observed
start boundary. Pressure coalesces optional status work, not media or commands.

Deliverable: real RPC/health consumers using the new ownership contract and
measured sub-ms core capture under stalled-owner/builder/writer tests.

## 4. Make freshness visible and complete regression verification

Project observation state/age into managed observations, Health and support bundles.
Hold last meter values visibly stale, expose refusals and distinguish stalled
diagnostics from stopped media. Existing output lifecycle failures stay authoritative.
Protect desired/applied settings from older observed revisions.

Run meaningful native concurrency and real-GPU pixel fault tests, managed lifecycle/
freshness/acknowledgment tests and required exact-head CI. Preserve failed runs and
negative-control evidence. Do not treat a known CI failure as a passing gate.

## 5. Qualify the installed candidate

Build a complete local candidate containing the reviewed #870 repair and #868
Program display handoff. Verify native/source hashes, installer retirement,
shortcut ownership, webcam registration and the DLL actually serving frames.
Do not interrupt an active show for installation.

First repeat the reported eight-Zoom-feed/one-capture Tiles setup and independent
camera/pixel checks. Then test the eight-feed/two-capture reference, both Program
depths, Takes/resize, grading/ROI/framer and enabled production outputs. Confirm
raw video rights and actual feed resolutions/cadences. Complete a 30–60 minute
real-meeting run with decoded A/V and per-output continuity evidence.

Deliverable: an evidence report with separate snapshot isolation, initiating-cause
attribution, each output/presentation boundary, startup and sustained-run verdicts.
Any miss fails that workload; missing evidence remains unverified. The owner's
visual confirmation is retained alongside measurements.

## Completion boundary

Review one focused #870 repair PR with its real consumers, tests, spec and evidence.
Close #870 only when its attribution and installed acceptance criteria are met.
Keep broader #517/#823 and unrelated #832/#754/#869 findings open where their own
criteria are unmet. Publish/install a beta through the normal complete-package
flow once its applicable release gates pass; a document or a clean counter-only
run is not release acceptance.
