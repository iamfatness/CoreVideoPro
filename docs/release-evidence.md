# Release validation and hardware evidence

Tag releases now call the complete reusable CI workflow at the tag's commit.
The Windows job runs native C++ tests, MediaCore, Control, WinUI, the bridge
smoke test and a WinUI publish build. The three .NET suites emit TRX under
`artifacts/test-results`; CI uploads them even when a suite fails. Missing CMake
or Visual Studio is a failed native gate. Run the same suites locally with
`npm run test:native-shell`, or the complete local gate with `npm run test:gate`.

Publication follows this dependency chain:

1. Required CI and version validation.
2. Build, package and sign the Windows MSIX. Generate `candidate.json` from the
   source SHA, actual CMake boolean flags, hashes of packaged DLL/EXE/JSON
   runtime files, and the signed package SHA-256. Upload this immutable candidate.
3. A controlled Windows rig downloads that candidate and exercises its bytes.
4. Validate hardware evidence and archive sanitized evidence/logs.
5. The publish job downloads the same candidate and evidence, rechecks both,
   and publishes the existing package with `candidate.json` and `verification.json`.

This avoids a hash cycle: evidence is collected **after** signing, and publication
does not rebuild or re-sign. An unsigned workflow-dispatch dry run cannot publish.
The private Zoom SDK and signing configuration remain required for the build.
Update-host publication still requires the existing manual upload step.

## Controlled rig provisioning

The workflow is fail-closed until these external prerequisites exist; this change
does not establish that a real Zoom or hardware session passed.

- Register a dedicated runner with labels `self-hosted`, `Windows`, `X64`, and
  `corevideo-release-rig`. Never use it to execute untrusted pull requests.
- Configure `production-hardware-validation` and `production-release` GitHub
  environments with the intended release/tag restrictions and reviewers.
- Set `COREVIDEO_HARDWARE_HARNESS` in the hardware environment to an absolute
  path to the rig-owned executable or PowerShell script. Restrict who can change
  this harness, variable, runner, and release workflow.
- Provision cameras, GPU/driver, Zoom test meeting participants, stream receivers,
  disposable disk/fault-injection storage and a clean Windows install/update target.
  The harness needs access to a clean target; an already-configured development
  machine alone does not prove installation behavior.
- Establish and retain a dated baseline with maximum memory slope, frame drop
  percentage, queue depth, and absolute A/V drift. Do not invent passing limits
  after seeing the candidate's results. Record the baseline ID in evidence.

The workflow calls the harness with named arguments:

```powershell
& $harness -CandidateManifest $manifest -PackagePath $signedMsix -EvidenceDirectory $freshDirectory
```

The harness installs and tests the package, exits nonzero on failure, and writes
`evidence.json` plus sanitized attachments to the fresh evidence directory.
The workflow allows four hours for the lane, including a mandatory two-hour
simultaneous recording/streaming soak. It never turns a missing harness, skipped
check, missing attachment, or stale report into success. No skip waivers exist in
schema version 1. If a packaged feature cannot pass a required test, publication
stops; changing a release's capability policy requires a reviewed workflow change.

## Evidence contract (schema version 1)

`evidence.json` is a JSON object with:

| Field | Requirement |
| --- | --- |
| `schemaVersion` | `1` |
| `sourceSha` | Full 40-character commit SHA from candidate |
| `configurationSha256` | Exact digest from candidate |
| `artifactSha256` | SHA-256 of the tested signed MSIX |
| `startedAt`, `completedAt` | ISO timestamps after candidate creation, in order, no future completion, completed within seven days |
| `environment` | Nonempty strings: `rigId`, `os`, `gpu`, `driver`, `zoomSdkVersion`, `runtimeVersions`, `baselineId` |
| `checks` | Unique check objects described below |

Each check has `id`, `status: "passed"`, and a nonempty `attachments` array of
`{ "path": "relative/sanitized-report.json", "sha256": "<64 lowercase hex characters>" }`.
The validator reads and hashes attachments; a link or claimed checksum alone is
insufficient. Attachments must be inside the evidence directory. Keep meeting
credentials, personal participant content and confidential SDK files out of
uploaded artifacts. Prefer sanitized measurements, logs and decoded media metadata;
retain sensitive source recordings on access-controlled rig storage.

Required check IDs:

- `zoom-multiparticipant-ingest`
- `program-iso-record-stream`
- `simultaneous-soak`
- `network-interruption-per-destination`
- `camera-unplug-replug`
- `core-process-termination`
- `zoom-process-termination`
- `disk-exhaustion`
- `shutdown-during-finalization`
- `decode-and-av-alignment`
- `clean-machine-install-update-launch`

`simultaneous-soak` additionally has `durationSeconds >= 7200` and `metrics` with
`memorySlopeMbPerHour`, `frameDropPercent`, `queueDepth`, `commandP95Ms`,
`commandMaxMs`, and `avDriftMs`. Each metric is `{ "value": number, "maximum": number }`;
both must be finite and nonnegative, with value at most maximum. Latency maximums
cannot exceed 250 ms p95 / 1000 ms maximum; use the approved baseline's numeric
limits for other metrics. The reported test interval must span the soak duration.
Reports should distinguish request acceptance latency from actual media effects.

Decode checks must inspect program and ISO streams, duration, timestamp order,
A/V alignment and intelligible content. A file-size check alone is not this check.
Fault reports should identify affected destinations, recovery behavior, finalization
outcome and the actual simulated or physical fault. Clean-install reports should
identify the clean target and prior version used for upgrade validation.

Validate a collected report locally:

```powershell
node scripts/release-evidence.mjs validate candidate.json evidence/evidence.json CoreVideoPro.msix verdict.json
npm run test:release-evidence
```

The validator verifies provenance consistency, completeness and thresholds; it
cannot independently prove that a human-controlled rig performed the reported
experiment. Trust rests on the protected harness/rig and retained reports.
There is currently no macOS publication workflow in `release.yml`; any future
macOS artifact must add an equivalent exact-artifact hardware gate before shipping.
