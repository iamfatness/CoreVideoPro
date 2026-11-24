# macOS Comms live runbook (plan Tasks 5 and 11)

Plan: `docs/superpowers/plans/2026-11-22-mac-comms-social-phase4.md`.

This page lists the exact steps for the two live sessions the Comms lane needs.
An agent prepared the steps but did not run them. Nothing here joins a meeting
unless a person starts it.

- **Task 5** is the send-threading spike plus the live engine proof. It drives
  the real engine directly, so it needs no shell build. It takes about 45 min
  and needs the owner plus one co-host. Two desktop-client guests are better
  than one.
- **Task 11** is the co-host acceptance on the installed signed build. It takes
  about 60 min and needs the owner, a co-host and one Zoom web-client guest.
  It needs the shell Comms tab (Tasks 6-10).

The roughly 30 s wait for video after a join is normal ramp-up. Do not chase it.

---

## Task 5: send-threading spike and live engine proof

### What it decides

1. **Send mode.** The question is whether `sendAudioDataToChannel:` can be
   called from the 20 ms TX pacer thread (`direct`), or must go through the main
   queue (`main`). The rule is decided by measurement:
   - ship `direct` if run A passes;
   - otherwise ship `main` if run B passes;
   - if neither passes, **stop** and report to the owner before the Comms page
     work continues.
2. **Audio join (owner decision N3).** If `comms_audio_join` shows
   `audioType: 0` after the join, meeting audio is not joined, MTG MIC stays
   off and talkback is silent. In that case **stop and raise N3**. Do not add
   `enableAutoJoinVoip:`.
3. **AudioReady.** Does `meeting_status 8` arrive mid-meeting? If it does, the
   bank must **not** be re-requested: the log must not contain a second
   `requesting 16 talkback channels`.
4. **Mic permission (TCC).** With access denied, the `mic.error` text must read
   "Microphone access is off for CoreVideo Pro. Turn it on in System Settings >
   Privacy & Security > Microphone.", and `peak_dbfs` must be null.

### Caveat on the mic-permission check

In this session the probe starts the engine from a terminal. TCC therefore
attributes the microphone use to the **terminal app** (Terminal, iTerm, …), not
to CoreVideo Pro.app. So the Task 5 TCC check proves that the gate reads and
reports the grant. Toggle the terminal's Microphone entry, not CoreVideo Pro's.
Whether the shell-launched engine (fork of the core, fork of the shell) reads
**CoreVideo Pro's** grant is the Task 11 item 10 check.

### Build once (agent or owner)

```sh
cd <worktree>
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
cmake --build native/build-engine --target corevideo-zoom-engine
scripts/make-macos-engine-bundle.sh --build-dir native/build-engine --link-sdk
ENGINE=$PWD/native/build-engine/corevideo-zoom-engine.app/Contents/MacOS/corevideo-zoom-engine
python3 scripts/qa/macos-engine-comms-probe.py --engine "$ENGINE" idle   # expect 4 PASS lines
```

### Before the meeting

- **Owner:** start a meeting from the account that authorised CoreVideo's
  public app key. The engine joins as a guest with that key, which can only
  guest-join meetings that this account hosts. Turn the waiting room OFF, or
  admit "CoreVideo Pro comms probe" when it appears.
- **Guest A** (the owner on a second desktop client, or the co-host) and
  **guest B**, both on the Zoom **desktop** client. Each starts a local
  recording with audio before run A. QuickTime "New Screen Recording" with the
  microphone option set to capture system audio, or any app that records what
  the Zoom client plays, will do.
- **Quit CoreVideo Pro** if it is running. The SDK refuses to initialise while
  another process holds one.

### Run A: direct mode, 10 min

```sh
python3 scripts/qa/macos-engine-comms-probe.py --engine "$ENGINE" meeting <MEETING_ID> [PASSCODE] \
  --minutes 10 --send-mode direct --slot 0 --log /tmp/comms-run-a.jsonl
```

1. When the probe prints `waiting for talkback`, the owner makes **"CoreVideo
   Pro comms probe"** co-host from the Zoom participants list.
2. The probe waits for `available=True`, waits 10 s for auto-assign, then turns
   on the 700 Hz tone and holds slot 0 for 10 min with a 1500 ms lease renewed
   every 500 ms.
   - Slot 0 is the first person auto-assign put on a channel. Note who that is
     from the `[comms_state]` lines; that person is "guest A" for the pass rule.
3. Midway, the operator turns the probe machine's Wi-Fi off for about 10 s, then
   back on. This produces the `meeting_status` sequence for question 3.

**PASS** requires all of these:
- `send_errors` is the same at start and end;
- `underruns` rises by at most 1 per minute;
- there is no `[comms_tick_exception]`;
- the engine is still running at the end;
- the 700 Hz tone is in guest A's recording and absent from guest B's.

### Run B: main mode, 10 min

Run the same command with `--send-mode main --log /tmp/comms-run-b.jsonl`. The
pass rule is the same as run A, plus every `[comms_send_hop]` line has
`p99Us < 20000`.

Run B is required even if run A passes, so the evidence has both numbers. If
time is short and run A passed, a 3-minute run B is enough.

### Private comms, live (2 min)

Rerun with `--minutes 1 --private`.
- **ON:** the engine's meeting mic is muted at once. Within 5 s the log shows
  either `silent virtual mic initialised` or the 5 s give-up line. Guest B's
  recording has no engine-mic audio while guest A still hears the tone.
- **OFF:** the log shows `left MUTED`, and `meeting_mic lamp: False` is printed.

### Mic permission (TCC), 2 min each

Rerun with `--minutes 1` twice:
1. With the terminal app's Microphone access OFF (System Settings > Privacy &
   Security > Microphone), the last `comms_state` `mic.error` must be the N1
   text and `mic.peak_dbfs` must be `null`.
2. With access ON, `mic.peak_dbfs` must be a number.

Then answer: did the engine's `authorizationStatus` agree with the toggle?

### Task 4 mutations (2 min each)

Rebuild the engine with each mutant, rerun with `--minutes 2`, and record
whether each turns red. Restore the file after each.

| Mutant | Edit in `native/zoom-engine/engine/engine-comms-policy.h` / `engine-comms-macos.mm` | Expected red |
|---|---|---|
| None read as unmuted | in `selfMuted`, add `case 0:` to the unmuted list | MicGuard never sees muted → wrong mic flow; `meeting_mic` lamp lies |
| UnMute before onMicInitialize | in `tick()`, treat `gi.initialised = true` | log shows the mic opened before `silent virtual mic initialised` |
| Skip decorator Shutdown | delete `if (s.decorator) s.decorator->Shutdown();` in `end_meeting` | main mode only: `droppedAfterShutdown` stays 0, and leave-while-keyed may send into a retired controller |

### Evidence

Copy the quoted probe lines into `docs/qa/<run-date>-mac-comms-send-spike.md`:
- the `meeting_status` sequence;
- `comms_audio_join`;
- start and end stats;
- every `comms_send_hop` line;
- the private and TCC lines;
- the mutation results.

Then set `kDefaultSendMode` in `native/zoom-engine/engine/main-queue-talkback-sdk.h`
to the chosen mode. Update `MainQueueTalkbackSdk.SendModeFromEnv` in the same
commit.

---

## Task 11: co-host acceptance (after Tasks 6-10)

Run on the installed, signed build launched with `COREVIDEO_SHELL_COMMS=1`,
with two desktop guests and one web-client guest. Mark each item PASS or FAIL
and attach its evidence.

1. **Page states:** "Not in a meeting" → waiting → "Talkback is not up" with the
   host hint (engine not yet co-host) → desk (after the owner grants co-host).
2. **Single key:** hold guest A's cell. Guest A's recording has the operator's
   voice and guest B's does not. The cell reads "on air".
3. **ALL CALL:** reaches both desktop guests.
4. **LATCH:** a latched press sticks across press and release.
5. **Force-quit while holding:** hold a key, then `kill -9` the shell. Guest A
   stops hearing the operator within 1.5 s (time it against A's recording).
6. **Switch away while holding:** with a key held, switching apps or changing
   tabs drops the key at once.
7. **Web-client guest:** shows "NO TALKBACK" in Edit talent. Assigning them is
   refused with the core's verbatim text.
8. **Private comms:**
   - ON: the operator's voice is absent from the meeting recording and present
     at the keyed guest.
   - ON → OFF: the operator stays muted, and the rail and ops line say so.
9. **No impact on video and recordings:** video, ISO and Program recording run
   throughout. `iso-pixel-verdict.py` passes on a 5-minute recording. During a
   60 s key hold, `realtimeEvidence` shows no new underruns or missed deadlines,
   and `zoomSubscriptionChurn` stays flat.
10. **Mic permission:** turn off CoreVideo Pro's Microphone access in System
    Settings. The settings drawer shows the N1 text and the meter is unlit.

Before and after the session, on a quiet machine, run:

```sh
python3 scripts/mac-show-drill.py --seconds 40 --load 8
```

This is owner decision N6: live 8-feed comms load stays unverified.
