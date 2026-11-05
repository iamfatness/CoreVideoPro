# Auto director policy qualification

Owner-requested repair: [#1146](https://github.com/iamfatness/CoreVideoPro/issues/1146).
Caption correction is separate (#1144). Existing product acceptance stays under
#668. This is an evidence record and scenario contract, not another work queue.

## Product improvement being tested

Previously, a native floor-based shot could be discarded by a shell camera-count
heuristic, leaving an unbound suggestion that Auto could not execute. Settings now
reach the native decision owner and the returned shot retains its exact people.
Panel eligibility counts guests rather than hosts/duplicate camera sources.
Turning off share priority continues guest-floor selection rather than disabling
actuation. A stale settings acknowledgment holds Program explicitly.

Interrupted candidate holds restart; an offline/invalid interval cannot count as
stable speech. The fixed Take-confidence minimum and additional timing holds are
visible. A blocked Take reports waiting, rather than claiming it is taking.

## Automated scenario evidence

| Scenario | Independent expected result | Evidence seam |
|---|---|---|
| Three speaking guests plus host; threshold 4; share priority off | Interview binds guests a/b, never host, even with an active share | Native DirectorPolicy test across thresholds 2–10 |
| Same floor; threshold 3 | Panel binds a/b/c; same count with threshold 4 yields a/b interview | Native selected scene and individual binding assertions |
| Share priority on | Share scene keeps guest a as its talker | Native selected scene and binding assertions |
| Two cameras, only one earned speaker; panel threshold 2 | Solo guest a; no invented second person | Native floor fixture |
| Policy command serialization and malformed replacement | Exact requested policy observed; invalid/partial update leaves prior whole policy | Real MediaCore command/snapshot and managed wire tests |
| Four roster entries but native interview | Exact a/b Preview, one Take after existing holds, no repeated Take | Serialized native payload through shell decision and coordinator host |
| Old/no policy or duplicate people | Explicit hold, confidence zero, no actionable fallback | Managed policy cases |
| Confidence/bindings/core/meeting interrupted for 20 seconds | Full 1.2-second binding hold must start again after recovery | Fake-clock coordinator cases |
| Manual Preview or mode override, pending Take, failure | Existing cancellation/ownership behavior remains | Full coordinator regression suite |

Snapshots and fake-host Take counts establish policy/actuation intent, not actual
installed pixels. Native command tests exercise the real parser and snapshot
assembler; kernel fixtures assert independently specified people and layouts.
The managed payload fixture is not a live native IPC or installed meeting trial.

Local results on the implementation branch: 1,952 WinUI tests and 3,140 MediaCore
tests passed. Native Windows Release **stub** build passed 12 Director/DirectorPolicy,
10 SpeakerFloor and 12 ContractParity cases. This is not production-device or
native full-suite qualification. The initial multi-pattern native filter matched
no tests and was rejected; the three separate filters above executed real cases.

Mutation check: restoring the old shell recommendation override made the
serialized-shot-to-Take test fail; removing invalid-candidate binding-hold reset
made all four interruption cases fail (5 failures total). Correct code was restored
and the complete managed suites passed again. An integration test initially caught
the policy command displacing the established Take/scene sequence; policy sync now
follows those commands and the original ordering assertion passes unchanged.

Local logs: `auto-director-winui-tests.log`, `auto-director-mediacore-tests.log`,
`auto-director-native-tests.log` and `auto-director-mutations.log` in the task
worktree. Hosted CI and installed operator acceptance are separate evidence.

## Installed acceptance still required

Use the exact integrated build with a host and three guests, populated source
frames and the normal desktop shortcut. Record the requested policy, acknowledged
policy, recommendation scene/bindings, Preview, Program, and actual output identity
at each stimulus. Test interview/solo/panel, threshold changes, share start/stop,
host backchannels, brief guest pauses, video loss/rejoin, and manual Preview/Take.
No duplicate person, host theft, unbound automatic shot, stale post-reconnect cut,
or captions activation is acceptable. Every refusal must name a meaningful reason.

Compare the operator's expected shots with observed Program; a passing unit suite
does not establish good live directing. Preserve all failed trials. Follow the
current 30-minute output gate and separate desktop UX tolerance from `CLAUDE.md`;
do not lower quality or waive frame delivery for automation. Installed acceptance
and overall RC readiness remain unverified until that trial is recorded.
