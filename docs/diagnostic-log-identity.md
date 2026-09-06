# Diagnostic log identity

Windows shell diagnostics retain their support-bundle filenames under
`%LOCALAPPDATA%\CoreVideoPro`: `launch.log`, `media-core.log`, and `perf.log`.
Each new entry includes an ISO timestamp with offset, the writing shell PID,
a process session identifier, assembly informational build version, and role.
The media-core file also contains forwarded child stderr: the prefix identifies
its shell owner, while the existing process-start record identifies the child PID.
Old entries may predate this metadata; do not infer their process identity.

Unit-test hosts write these files under `test-logs/<session>/` instead. Detection
uses loaded CoreVideoPro test assemblies or test-runner assemblies at the first
log access. Existing application support bundles continue to include only the
standard application log paths. Attach a test session directory separately when
reporting a test failure. This change does not remove historical test entries
already present in application logs.

Exception call sites should use `LaunchLog.WriteException(context, exception,
requestId)` or `DiagnosticLog.WriteException(fileName, context, exception,
requestId)`. These preserve the full exception including inner exceptions and
stack frames, plus request correlation when supplied. Keep context stable and
free of request payloads or credentials. Repeated context/type/message errors
emit the first occurrence and at most one example every ten seconds; the next
emitted example reports the number suppressed since the previous example. A
burst that ends within the window has no later summary until that error recurs.
The limiter bounds its dictionary to 256 distinct keys; reaching the bound resets
buckets so new failures remain visible. Normal transition logging is unchanged.

Performance timestamps describe the point named in each diagnostic, not proof of
presentation on a display. Build version identifies the assembled code and may
include source revision when supplied by the build; it is not an artifact hash.
