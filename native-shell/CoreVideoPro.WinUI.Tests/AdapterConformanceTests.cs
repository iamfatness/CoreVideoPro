using System.Text;
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
public sealed class AdapterConformanceTests
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

        // KNOWN DEFECT, PINNED SO IT CANNOT QUIETLY GET WORSE (or quietly disappear).
        //
        // Three cases select a look, and in each of them the engine emits
        // `setPreview({kind:"look", lookId})` BEFORE the `applyLook` that first tells the shell
        // which scene preset that look renders through — in the same tick, one seq apart. The
        // adapter can only learn `lookId -> scenePreset` from `applyLook` (spec §8 gives it no
        // other source), so it refuses the setPreview outright. Nothing on screen is wrong: the
        // `applyLook` a line later cues the very same scene to preview with all four routes, which
        // is what the refused command was asking for. But it is a real cross-process ordering
        // mismatch that neither side's own tests can see — the engine's suite asserts on a recorder
        // that has no such cache, and `OhgHostAdapterTests` feeds the adapter hand-written commands
        // in whatever order it chose — and it is exactly the class of finding this test exists for.
        //
        // It is NOT fixed here: the fix is either an engine emission-order change or a spec change
        // to `setPreview`'s look row, both outside Task 13. Pinned verbatim so a change in either
        // direction reds this test and gets read.
        Assert.Equal(
            new[]
            {
                "20 setPreview: setPreview: look 'conformance.panel' has not been applied yet",
                "55 setPreview: setPreview: look 'conformance.panel' has not been applied yet",
                "71 setPreview: setPreview: look 'conformance.panel' has not been applied yet"
            },
            run.Refusals);

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
        long StaleHostCommandsDropped);

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
                logs.Add(line.Message);

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
                    adapter = new OhgHostAdapter(facade, shell, _ => { });
                    return;
                }

                if (!line.Message.StartsWith("conformance: ", StringComparison.Ordinal)) return;

                CloseCase();
                if (line.Message.Contains('/', StringComparison.Ordinal)) finished.TrySetResult();
            };

            supervisor.HostCommandReceived += command =>
            {
                if (adapter is null) return;

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
            Assert.True(completed == finished.Task,
                "the show engine never printed its conformance tally; logs so far: " + string.Join(" | ", logs));

            await supervisor.StopAsync();
            CloseCase();

            return new ConformanceRun(logs, byCase, refusals, supervisor.StaleHostCommandsDropped);
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
