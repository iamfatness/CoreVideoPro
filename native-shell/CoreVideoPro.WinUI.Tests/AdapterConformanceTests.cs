using System.Diagnostics;
using System.Text;
using System.Text.RegularExpressions;
using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;
using Xunit;
using Xunit.Abstractions;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Records every <see cref="IOhgHostFacade"/> call as a <c>"&lt;Method&gt;(&lt;args&gt;)"</c> string
/// and says YES to everything a healthy shell would: every scene exists, every route the adapter
/// asks for is present, every slot 1..10 is assignable and named, every transition is known, and a
/// take always succeeds. That permissiveness is the point — the conformance run is measuring what
/// the ADAPTER converts each host command into, so a facade that refused would fold adapter
/// behaviour and shell state into one unreadable result.
/// </summary>
internal sealed class RecordingOhgHostFacade : IOhgHostFacade
{
    private readonly List<string> _records = new();

    public IReadOnlyList<string> Records => _records;

    public int ShowInputCount => 10;

    public bool CanTake
    {
        get
        {
            _records.Add("CanTake");
            return true;
        }
    }

    public bool AssignZoomParticipant(int slot, string? participantId)
    {
        _records.Add($"AssignZoomParticipant({slot}, {participantId ?? "null"})");
        return slot >= 1 && slot <= ShowInputCount;
    }

    public bool SetInputDisplayName(int slot, string name)
    {
        _records.Add($"SetInputDisplayName({slot}, {name})");
        return true;
    }

    public bool SetInputLowerThirdTitle(int slot, string title)
    {
        _records.Add($"SetInputLowerThirdTitle({slot}, {title})");
        return true;
    }

    public bool SceneExists(string sceneId)
    {
        _records.Add($"SceneExists({sceneId})");
        return true;
    }

    /// <summary>Route keys are rendered SORTED. The adapter builds them in a <see cref="Dictionary{TKey,TValue}"/>,
    /// whose enumeration order is an implementation detail — pinning it would make the goldens fail
    /// for a reason that has nothing to do with which guest went where.</summary>
    public IReadOnlyList<string> CueSceneWithRoutes(string sceneId, IReadOnlyDictionary<string, int?> routeSlots)
    {
        var rendered = string.Join(", ", routeSlots
            .OrderBy(pair => pair.Key, StringComparer.Ordinal)
            .Select(pair => $"{pair.Key}={(pair.Value.HasValue ? pair.Value.Value.ToString() : "null")}"));
        _records.Add($"CueSceneWithRoutes({sceneId}, {rendered})");
        return Array.Empty<string>();
    }

    public Task<bool> TakeAsync(string transition)
    {
        _records.Add($"TakeAsync({transition})");
        return Task.FromResult(true);
    }

    public bool IsKnownTransition(string transition)
    {
        _records.Add($"IsKnownTransition({transition})");
        return true;
    }

    public void SetCaption(string text) => _records.Add($"SetCaption({text})");

    public void ReportStatus(string line) => _records.Add($"ReportStatus({line})");
}

/// <summary>
/// Plan 7a Task 13 — spec §11's "conformance" row, and the discharge of Plan 6's carried
/// "per-shell adapter conformance" debt for the Windows shell.
///
/// <para><b>This is the first test in the plan that runs the REAL parts together.</b> A real
/// <c>node.exe</c> runs the real built <c>dist/host/main.js</c> in its <c>--conformance</c> mode,
/// spawned by the real <see cref="ProcessShowEngineChildFactory"/> under the real
/// <see cref="ShowEngineSupervisor"/>, and every <c>hostCommand</c> it emits is pushed through the
/// real <see cref="OhgHostAdapter"/> — the same class the shell uses on a live show. Only the two
/// ends are stand-ins: the engine's cases are synthetic (that is what a conformance case IS) and
/// the shell is a <see cref="RecordingOhgHostFacade"/>.</para>
///
/// <para><b>What it proves that nothing else does.</b> The engine's own conformance run (vitest,
/// <c>verify-dist-barrel.mjs</c>) asserts on what a RECORDER received; <see cref="OhgHostAdapterTests"/>
/// asserts the adapter's mapping against hand-written wire fixtures. Neither one proves those wire
/// fixtures are what the engine actually emits. This test closes that gap: the bytes the adapter
/// parses here were serialized by the engine, over a pipe, in a different process, in a different
/// language.</para>
///
/// <para><b>What it still does not prove</b> (the same honest limit <c>conformance.ts</c>'s header
/// states): the facade is a recorder, so a shell that received the right instruction and ignored it
/// is conformant here. Adapter BEHAVIOUR against a live ViewModel is Task 11's
/// <c>StudioViewModelOhgFacade</c> work, not this.</para>
///
/// <para><b>Skipping.</b> Node and a built <c>dist</c> are environment, not code, so their absence
/// is a printed skip rather than a red — but a printed one: a silently skipped integration test is
/// a guard that looks like coverage and isn't.</para>
/// </summary>
[Trait("Category", "Integration")]
public sealed partial class AdapterConformanceTests
{
    private readonly ITestOutputHelper _output;

    public AdapterConformanceTests(ITestOutputHelper output) => _output = output;

    [Fact]
    public async Task EveryConformanceCaseDrivesTheRealAdapterToItsGoldenFacadeSequence()
    {
        var repoRoot = FindRepoRoot();
        if (repoRoot is null)
        {
            _output.WriteLine("SKIPPED: no show-engine/package.json found walking up from " + AppContext.BaseDirectory);
            return;
        }

        var entryScript = Path.Combine(repoRoot, "show-engine", "dist", "host", "main.js");
        if (!File.Exists(entryScript))
        {
            _output.WriteLine($"SKIPPED: {entryScript} is not built — run `npm run build` in show-engine/");
            return;
        }

        var nodeExe = FindNodeOnPath();
        if (nodeExe is null)
        {
            _output.WriteLine("SKIPPED: node.exe is not on PATH");
            return;
        }

        _output.WriteLine($"node:  {nodeExe}");
        _output.WriteLine($"entry: {entryScript}");

        var run = await RunConformanceAsync(nodeExe, entryScript, repoRoot);

        foreach (var line in run.Logs)
        {
            _output.WriteLine(line);
        }

        // NO REFUSALS AT ALL. Every command the engine emits under CONFORMANCE_CONFIG is one this
        // shell can carry out, and the adapter must carry all of them out.
        //
        // This assertion is where fix round 1's real defect surfaced. Three cases select a look,
        // and in each the engine emits `setPreview({kind:"look", lookId})` one seq BEFORE the
        // `applyLook` that names that look's scene preset. An adapter that learned
        // `lookId -> scenePreset` only from `applyLook` refused all three — i.e. it refused the
        // FIRST cue of every look on every real show, a defect neither side's own tests could see
        // (the engine's suite asserts on a recorder that keeps no such map, and
        // `OhgHostAdapterTests` fed the adapter hand-written commands in an order it chose itself).
        // The mapping was in `config.engine.looks[]` all along; the adapter is now seeded from it
        // (see the `lookPresets` argument below), so this list is empty.
        Assert.Empty(run.Refusals);

        // Not one host command arrived outside a `begin`/`ok` bracket. The whole per-case bucketing
        // rests on that framing, and a command counted into no case would silently shrink a golden.
        Assert.Equal(0, run.UnbracketedHostCommands);

        Assert.Contains($"conformance: {OhgConformanceGoldens.Expected.Count}/{OhgConformanceGoldens.Expected.Count}", run.Logs);
        Assert.Equal(0, run.StaleHostCommandsDropped);

        // Every golden case ran, and no case ran that has no golden — a case added engine-side with
        // no golden here would otherwise pass by never being looked at.
        Assert.Equal(
            OhgConformanceGoldens.Expected.Keys.OrderBy(name => name, StringComparer.Ordinal),
            run.FacadeCallsByCase.Keys.OrderBy(name => name, StringComparer.Ordinal));

        foreach (var (caseName, expected) in OhgConformanceGoldens.Expected)
        {
            _output.WriteLine($"--- {caseName}");
            foreach (var call in run.FacadeCallsByCase[caseName])
            {
                _output.WriteLine("    " + call);
            }

            Assert.Equal(expected, run.FacadeCallsByCase[caseName]);
        }
    }

    // ---- the run ------------------------------------------------------------------------

    private sealed record ConformanceRun(
        IReadOnlyList<string> Logs,
        IReadOnlyDictionary<string, IReadOnlyList<string>> FacadeCallsByCase,
        IReadOnlyList<string> Refusals,
        long StaleHostCommandsDropped,
        int UnbracketedHostCommands);

    /// <summary>The tally that ends a run: <c>conformance: &lt;passed&gt;/&lt;total&gt;</c>, and nothing else.</summary>
    [GeneratedRegex(@"^conformance: \d+/\d+$")]
    private static partial Regex TallyLine();

    /// <summary>
    /// What the shell would have read out of <c>config.engine.looks[]</c> — the ONE look
    /// <c>CONFORMANCE_CONFIG</c> declares (<c>show-engine/src/conformance.ts</c>:
    /// <c>CONFORMANCE_LOOK_ID</c> / <c>CONFORMANCE_SCENE_PRESET</c>).
    ///
    /// <para>It is written out here rather than parsed from the temp config because the
    /// <c>--conformance</c> mode builds its engines from <c>CONFORMANCE_CONFIG</c> internally and
    /// ignores <c>--config</c> entirely; a seed read from a file the engine never opened would be
    /// a fiction that happened to agree. If either constant changes engine-side, the look case's
    /// <c>applyLook</c> carries the new preset and this map goes stale — which shows up as a
    /// refusal, i.e. loudly, on the <c>Assert.Empty(run.Refusals)</c> above.</para>
    /// </summary>
    private static readonly IReadOnlyDictionary<string, string> ConformanceLookPresets =
        new Dictionary<string, string>(StringComparer.Ordinal) { ["conformance.panel"] = "conformance-scene" };

    /// <summary>All four presets set and <c>DriveHost = true</c>: shadow mode records instead of
    /// applying, so a conformance run in shadow mode would assert on the adapter's LOG rather than
    /// on its mapping, which is the thing under test.</summary>
    private static ShowShellConfig ShellConfig() => new()
    {
        DriveHost = true,
        DefaultTransition = "cut",
        Presets = new ShowPresetScenes
        {
            Solo = "solo-scene",
            ActiveSpeaker = "active-speaker-scene",
            Black = "black-scene",
            Gallery = "gallery-scene"
        }
    };

    private async Task<ConformanceRun> RunConformanceAsync(string nodeExe, string entryScript, string repoRoot)
    {
        var tempDir = Directory.CreateTempSubdirectory("cvp-ohg-conformance-").FullName;
        try
        {
            // The mode builds its engines from CONFORMANCE_CONFIG internally (the cases name that
            // exact config), so this file is never read. It exists because the real child ALWAYS
            // passes --config, and this test spawns through the real child on purpose.
            var configPath = Path.Combine(tempDir, "show-config.json");
            await File.WriteAllTextAsync(configPath, "{}", new UTF8Encoding(false));

            var shell = ShellConfig();
            var logs = new List<string>();
            var refusals = new List<string>();
            var byCase = new Dictionary<string, IReadOnlyList<string>>(StringComparer.Ordinal);
            var finished = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

            // Written and read ONLY on the supervisor's reader thread (log lines and host commands
            // are raised from it, in wire order), so the per-case bucketing below cannot interleave.
            string? currentCase = null;
            RecordingOhgHostFacade? facade = null;
            OhgHostAdapter? adapter = null;
            var unbracketed = 0;

            void CloseCase()
            {
                if (currentCase is null || facade is null) return;
                byCase[currentCase] = facade.Records.ToArray();
                currentCase = null;
                facade = null;
                adapter = null;
            }

            using var supervisor = new ShowEngineSupervisor(
                new ProcessShowEngineChildFactory(new ShowEngineLog(Path.Combine(tempDir, "show-engine.log"))),
                // Zero restarts: the mode exits when we stop it, and a respawn would run the whole
                // suite a second time into the same buckets.
                new ShowEngineRestartPolicy(maxConsecutiveFailures: 0),
                (delay, ct) => Task.Delay(delay, ct),
                () => DateTimeOffset.UtcNow,
                new ShowEngineSupervisorOptions
                {
                    HandshakeTimeout = TimeSpan.FromSeconds(30),
                    RequestTimeout = TimeSpan.FromSeconds(10),
                    // The conformance mode answers pings, but it is not a show: nothing here needs
                    // hang detection, and a heartbeat that fired mid-run would only add noise.
                    HeartbeatInterval = TimeSpan.FromMinutes(10)
                });

            supervisor.LogReceived += line =>
            {
                lock (logs) logs.Add(line.Message);

                const string beginPrefix = "conformance: begin ";
                if (line.Message.StartsWith(beginPrefix, StringComparison.Ordinal))
                {
                    CloseCase();
                    currentCase = line.Message[beginPrefix.Length..];
                    facade = new RecordingOhgHostFacade();
                    // A FRESH adapter per case, mirroring the suite's own "a fresh engine per case"
                    // contract: the adapter carries report-once state (the gallery note, the
                    // missing-route set), so one adapter across the run would make case N's golden
                    // depend on whether case N-1 had already spoken.
                    adapter = new OhgHostAdapter(facade, shell, _ => { }, ConformanceLookPresets);
                    return;
                }

                if (!line.Message.StartsWith("conformance: ", StringComparison.Ordinal)) return;

                CloseCase();
                // The tally, matched exactly — a case whose NAME happened to contain a slash would
                // otherwise end the run early and truncate every bucket after it.
                if (TallyLine().IsMatch(line.Message)) finished.TrySetResult();
            };

            supervisor.HostCommandReceived += command =>
            {
                if (adapter is null)
                {
                    unbracketed += 1;
                    return;
                }

                // Awaited synchronously ON the reader thread on purpose: arrival order IS the
                // contract, and the facade completes every task synchronously so nothing blocks.
                var refusal = adapter.ApplyAsync(command).GetAwaiter().GetResult();
                if (refusal is not null) refusals.Add($"{command.Seq} {command.Name}: {refusal}");
            };

            var request = new ShowEngineSpawnRequest(
                nodeExe,
                entryScript,
                configPath,
                Path.Combine(repoRoot, "show-engine"),
                new Dictionary<string, string>(StringComparer.Ordinal))
            {
                ExtraArgs = new[] { "--conformance" }
            };

            using var cts = new CancellationTokenSource(TimeSpan.FromMinutes(2));
            await supervisor.StartAsync(request, cts.Token);
            Assert.Equal(ShowEngineState.Running, supervisor.Health.State);

            // THE START SIGNAL, and the reason the mode waits for one. The supervisor's stdout
            // READER runs on a different thread from the handshake parse that sets its current
            // generation, and it drops every host command whose generation does not match — so a
            // conformance mode that began the instant it had announced could have its first
            // commands read while that field was still 0 and silently counted as stale. Under this
            // project's full 940-test run that is exactly what happened: 19 dropped. A round-tripped
            // request is proof the handshake is fully parsed (SendAsync refuses unless Running),
            // and the engine starts its cases on receiving it.
            (await supervisor.SendAsync("ping", null, cts.Token)).Dispose();

            var completed = await Task.WhenAny(finished.Task, Task.Delay(TimeSpan.FromMinutes(2), cts.Token));
            if (completed != finished.Task)
            {
                // `logs` is appended to by the reader thread, which is still running — formatting it
                // directly races an Add and can throw out of the failure message itself.
                string sofar;
                lock (logs) sofar = string.Join(" | ", logs.ToArray());
                Assert.Fail("the show engine never printed its conformance tally; logs so far: " + sofar);
            }

            await supervisor.StopAsync();
            CloseCase();

            lock (logs)
            {
                return new ConformanceRun(
                    logs.ToArray(), byCase, refusals.ToArray(), supervisor.StaleHostCommandsDropped, unbracketed);
            }
        }
        finally
        {
            try { Directory.Delete(tempDir, recursive: true); } catch { /* best effort */ }
        }
    }

    // ---- the exit-code contract ---------------------------------------------------------

    /// <summary>
    /// <c>--conformance</c> exits <b>0 iff every case passed</b> — asserted against the process
    /// itself, because nothing else can.
    ///
    /// <para>The supervisor path above is the real integration, but <see cref="ShowEngineSupervisor"/>
    /// exposes no exit code (it has no use for one), so the contract that a CI job or a packaging
    /// script would actually gate on is unobserved there. This drives the same built entry point as
    /// a plain <see cref="Process"/>, feeding stdin the same three requests the supervisor sends —
    /// a <c>handshake</c>, then the request the mode waits for before running its cases, then
    /// <c>shutdown</c> — and reads the exit code.</para>
    ///
    /// <para>It deliberately does NOT parse the output: what the run printed is the other test's
    /// subject, and a second copy of those assertions here would just be a place for them to drift.
    /// The one thing read out of stdout is the tally line, and only to make a non-zero exit
    /// diagnosable.</para>
    /// </summary>
    [Fact]
    public async Task TheConformanceModeExitsZeroWhenEveryCasePassed()
    {
        var repoRoot = FindRepoRoot();
        if (repoRoot is null)
        {
            _output.WriteLine("SKIPPED: no show-engine/package.json found walking up from " + AppContext.BaseDirectory);
            return;
        }

        var entryScript = Path.Combine(repoRoot, "show-engine", "dist", "host", "main.js");
        if (!File.Exists(entryScript))
        {
            _output.WriteLine($"SKIPPED: {entryScript} is not built — run `npm run build` in show-engine/");
            return;
        }

        var nodeExe = FindNodeOnPath();
        if (nodeExe is null)
        {
            _output.WriteLine("SKIPPED: node.exe is not on PATH");
            return;
        }

        var tempDir = Directory.CreateTempSubdirectory("cvp-ohg-conformance-exit-").FullName;
        try
        {
            var configPath = Path.Combine(tempDir, "show-config.json");
            await File.WriteAllTextAsync(configPath, "{}", new UTF8Encoding(false));

            var startInfo = new ProcessStartInfo
            {
                FileName = nodeExe,
                WorkingDirectory = Path.Combine(repoRoot, "show-engine"),
                UseShellExecute = false,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardInputEncoding = new UTF8Encoding(false),
                StandardOutputEncoding = new UTF8Encoding(false),
                CreateNoWindow = true
            };
            startInfo.ArgumentList.Add(entryScript);
            startInfo.ArgumentList.Add("--config");
            startInfo.ArgumentList.Add(configPath);
            startInfo.ArgumentList.Add("--generation");
            startInfo.ArgumentList.Add("1");
            startInfo.ArgumentList.Add("--conformance");

            using var process = Process.Start(startInfo)
                ?? throw new InvalidOperationException("node did not start");

            using var kill = new CancellationTokenSource(TimeSpan.FromMinutes(2));
            await using (var stdin = process.StandardInput)
            {
                // The mode answers any well-formed request `ok`; the FIRST one is also its start
                // signal (it holds its cases until the parent has spoken — see main.ts). `shutdown`
                // is what releases it to exit once the tally is printed.
                await stdin.WriteLineAsync("""{"id":"se-1","type":"handshake"}""");
                await stdin.WriteLineAsync("""{"id":"se-2","type":"ping"}""");
                await stdin.WriteLineAsync("""{"id":"se-3","type":"shutdown"}""");
                await stdin.FlushAsync(kill.Token);
            }

            string stdout;
            string stderr;
            try
            {
                // ReadToEndAsync is itself cancellable, and a child that wedges with stdout still
                // open cancels HERE rather than at WaitForExitAsync — leaving node running for the
                // rest of the test session. The kill belongs to the whole read/wait span, not to
                // the wait alone.
                stdout = await process.StandardOutput.ReadToEndAsync(kill.Token);
                stderr = await process.StandardError.ReadToEndAsync(kill.Token);
                await process.WaitForExitAsync(kill.Token);
            }
            catch (OperationCanceledException)
            {
                Assert.Fail("the conformance mode did not exit within 2 minutes");
                throw;   // unreachable; Assert.Fail always throws
            }
            finally
            {
                try
                {
                    if (!process.HasExited) process.Kill(entireProcessTree: true);
                }
                catch { /* best effort - it may have exited between the check and the kill */ }
            }

            var tally = stdout
                .Split('\n')
                .Select(line => line.Trim())
                .LastOrDefault(line => line.Contains("conformance: ", StringComparison.Ordinal));
            _output.WriteLine("tally line: " + (tally ?? "<none>"));
            if (stderr.Length > 0) _output.WriteLine("stderr: " + stderr);

            Assert.Equal(0, process.ExitCode);
        }
        finally
        {
            try { Directory.Delete(tempDir, recursive: true); } catch { /* best effort */ }
        }
    }

    // ---- environment probes -------------------------------------------------------------

    /// <summary>Walk up from the test assembly until a directory holds <c>show-engine\package.json</c>
    /// — the same "dev layout" shape <see cref="ShowEnginePaths"/>'s dev candidate assumes.</summary>
    private static string? FindRepoRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            if (File.Exists(Path.Combine(directory.FullName, "show-engine", "package.json")))
            {
                return directory.FullName;
            }

            directory = directory.Parent;
        }

        return null;
    }

    private static string? FindNodeOnPath()
    {
        var path = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrEmpty(path)) return null;

        foreach (var directory in path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            string candidate;
            try
            {
                candidate = Path.Combine(directory.Trim('"'), "node.exe");
            }
            catch (ArgumentException)
            {
                continue;   // an unusable PATH entry is not a reason to fail the probe
            }

            if (File.Exists(candidate)) return candidate;
        }

        return null;
    }
}
