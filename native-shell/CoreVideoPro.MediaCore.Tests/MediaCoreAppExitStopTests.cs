using System.Diagnostics;
using System.Reflection;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// T1.8 (#461): the app-exit stop closes the core's stdin (its quit signal — JsonRpcServer's reader
/// hits EOF and <c>main</c> returns), waits a bounded grace for the core to exit on its own, and only
/// then kill-trees it. Fake cores are small node scripts, like the handshake tests.
///
/// <para><b>No wall-clock assertions (fix round 1, finding 3).</b> These run beside a parallel
/// build under xUnit's class parallelism, so every proof is STRUCTURAL: a process that ran its own
/// exit path writes <c>exit:&lt;code&gt;</c> from <c>process.on('exit')</c>, and one ended by
/// TerminateProcess cannot. Graces are generous (a passing run still returns the moment the child
/// exits); the only time bounds left are hang guards, and one deterministic lower bound
/// (<c>WaitForExit(grace)</c> cannot return early for a live process).</para>
/// </summary>
public sealed class MediaCoreAppExitStopTests
{
    private static readonly TimeSpan Generous = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan HangGuard = TimeSpan.FromSeconds(90);
    private static readonly TimeSpan ProductionGrace = TimeSpan.FromSeconds(2);

    private const string Handshake =
        "console.log(JSON.stringify({id:'handshake',ok:true,type:'handshake',protocolVersion:{major:1,minor:0},profile:{name:'fake',renderer:'software',maxProgramResolution:'1920x1080'}}));";

    private sealed record Run(MediaCoreExitOutcome Outcome, TimeSpan Elapsed, string Trace);

    private static async Task<Run> RunAsync(
        string body,
        Func<MediaCoreSupervisor, MediaCoreExitOutcome> stop,
        Func<string, Task>? beforeStop = null)
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-app-exit-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "core.cjs");
            var trace = Path.Combine(directory, "trace.txt");
            // A process that runs its own exit path records its code; a killed one (TerminateProcess)
            // cannot, so "exit:" in the trace is the proof of which way it ended.
            await File.WriteAllTextAsync(script,
                "const fs = require('node:fs');\n" +
                "process.on('exit', code => fs.appendFileSync(process.env.COREVIDEO_EXIT_TRACE, 'exit:' + code + '\\n'));\n" +
                Handshake + "\n" + body);
            await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                Command = "node", Args = [script], WorkingDirectory = Path.GetTempPath(),
                Environment = new Dictionary<string, string> { ["COREVIDEO_EXIT_TRACE"] = trace },
                HandshakeRequestTimeoutMs = 30000, RequestTimeoutMs = 30000, FrameDrainIntervalMs = 100000, MaxRestarts = 0
            });
            await supervisor.StartAsync().WaitAsync(Generous);
            var process = (Process)typeof(MediaCoreSupervisor)
                .GetField("_process", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(supervisor)!;
            using var child = Process.GetProcessById(process.Id);
            if (beforeStop is not null)
            {
                await beforeStop(trace);
            }

            var stopwatch = Stopwatch.StartNew();
            var outcome = await Task.Run(() => stop(supervisor)).WaitAsync(HangGuard);
            stopwatch.Stop();
            await child.WaitForExitAsync().WaitAsync(HangGuard);
            Assert.False(supervisor.Running);
            var traceText = await TryReadTraceAsync(trace) ?? string.Empty;
            return new Run(outcome, stopwatch.Elapsed, traceText);
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); } catch { /* a dying child can hold the cwd briefly */ }
        }
    }

    /// <summary>
    /// #834: read the trace while the child may be appending to it. <c>File.ReadAllTextAsync</c> opens
    /// with <c>FileShare.Read</c>, which Windows refuses (a sharing violation) whenever the child holds
    /// the file open for writing, so the read must share read/write/delete. A residual conflict
    /// (the child's open is exclusive for an instant) is not a verdict: the caller retries until its
    /// own deadline. Returns null when the file is absent or momentarily locked.
    /// </summary>
    private static bool IsSharingViolation(IOException ex)
    {
        // ERROR_SHARING_VIOLATION (32) and ERROR_LOCK_VIOLATION (33) in the low word of the HRESULT.
        var code = ex.HResult & 0xFFFF;
        return code is 32 or 33;
    }

    internal static async Task<string?> TryReadTraceAsync(string trace)
    {
        try
        {
            await using var stream = new FileStream(
                trace, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete,
                bufferSize: 4096, useAsync: true);
            using var reader = new StreamReader(stream);
            return await reader.ReadToEndAsync();
        }
        catch (FileNotFoundException) { return null; }
        catch (DirectoryNotFoundException) { return null; }
        // Only a sharing/lock violation is a retryable "not now". Anything else is a real read
        // failure and must surface, or a negative assertion on the trace passes vacuously.
        catch (IOException ex) when (IsSharingViolation(ex)) { return null; }
    }

    internal static async Task<string> WaitForTraceAsync(string trace, string marker, TimeSpan? limit = null)
    {
        var bound = limit ?? HangGuard;
        var deadline = Stopwatch.StartNew();
        while (deadline.Elapsed < bound)
        {
            var text = await TryReadTraceAsync(trace);
            if (text is not null && text.Contains(marker, StringComparison.Ordinal)) return text;

            await Task.Delay(50);
        }

        throw new TimeoutException($"'{marker}' never appeared in the trace");
    }

    [Fact]
    public async Task TraceWaitSurvivesAChildHoldingTheFileOpenForWriting()
    {
        var path = Path.Combine(Path.GetTempPath(), "corevideo-trace-" + Guid.NewGuid().ToString("N") + ".txt");
        try
        {
            // The child's appendFileSync keeps a write handle open; the old FileShare.Read reader threw here.
            await using var writer = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete);
            await writer.WriteAsync("grandchild:42\n"u8.ToArray());
            await writer.FlushAsync();

            var text = await WaitForTraceAsync(path, "grandchild:", TimeSpan.FromSeconds(10));
            Assert.Contains("grandchild:42", text);
        }
        finally { try { File.Delete(path); } catch { } }
    }

    [Fact]
    public async Task TraceWaitRetriesThroughAnExclusiveLockInsteadOfThrowing()
    {
        var path = Path.Combine(Path.GetTempPath(), "corevideo-trace-" + Guid.NewGuid().ToString("N") + ".txt");
        try
        {
            var exclusive = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None);
            await exclusive.WriteAsync("eof-ignored\n"u8.ToArray());
            await exclusive.FlushAsync();
            var wait = WaitForTraceAsync(path, "eof-ignored", TimeSpan.FromSeconds(20));
            await Task.Delay(300); // several retries land on the locked file
            Assert.False(wait.IsCompleted);
            await exclusive.DisposeAsync();
            Assert.Contains("eof-ignored", await wait);
        }
        finally { try { File.Delete(path); } catch { } }
    }

    [Fact]
    public async Task ATraceReadFailureThatIsNotASharingConflictSurfaces()
    {
        // A directory where the trace file should be: a real I/O error (access denied), not a
        // sharing conflict. It must throw, not read as an empty trace.
        var path = Path.Combine(Path.GetTempPath(), "corevideo-trace-" + Guid.NewGuid().ToString("N") + ".txt");
        Directory.CreateDirectory(path);
        try
        {
            await Assert.ThrowsAnyAsync<Exception>(() => TryReadTraceAsync(path));
        }
        finally { try { Directory.Delete(path); } catch { } }
    }

    [Fact]
    public async Task TraceWaitStillTimesOutWhenTheMarkerNeverAppears()
    {
        var path = Path.Combine(Path.GetTempPath(), "corevideo-trace-" + Guid.NewGuid().ToString("N") + ".txt");
        try
        {
            await File.WriteAllTextAsync(path, "something-else\n");
            await Assert.ThrowsAsync<TimeoutException>(() => WaitForTraceAsync(path, "grandchild:", TimeSpan.FromMilliseconds(400)));
        }
        finally { try { File.Delete(path); } catch { } }
    }

    [Fact]
    public async Task CoreThatExitsOnStdinEofIsNotKilled()
    {
        var result = await RunAsync("""
            process.stdin.resume();
            process.stdin.on('end', () => {
              fs.appendFileSync(process.env.COREVIDEO_EXIT_TRACE, 'eof\n');
              // Stand-in for the core joining its threads and finalizing on the way out.
              setTimeout(() => process.exit(0), 200);
            });
            """, supervisor => supervisor.StopForAppExit(Generous));

        Assert.Equal(MediaCoreExitOutcome.ExitedOnItsOwn, result.Outcome);
        Assert.Contains("eof", result.Trace);
        Assert.Contains("exit:0", result.Trace);
    }

    [Fact]
    public async Task CoreThatIgnoresEofIsKilledAfterTheGrace()
    {
        var result = await RunAsync("""
            process.stdin.resume();
            process.stdin.on('end', () => fs.appendFileSync(process.env.COREVIDEO_EXIT_TRACE, 'eof-ignored\n'));
            setInterval(() => {}, 1000);
            """, supervisor => supervisor.StopForAppExit(ProductionGrace));

        Assert.Equal(MediaCoreExitOutcome.Killed, result.Outcome);
        // Structural: TerminateProcess leaves no exit line. (EOF delivery itself is proven by the
        // test above; "eof-ignored" is diagnostic only and not asserted.)
        Assert.DoesNotContain("exit:", result.Trace);
        // Deterministic: WaitForExit(grace) cannot return early for a process that is still alive.
        Assert.True(result.Elapsed >= ProductionGrace - TimeSpan.FromMilliseconds(100),
            $"killed before the grace ran out ({result.Elapsed})");
    }

    [Fact]
    public async Task StdoutKeepsDrainingDuringTheGraceSoTheCoreCanFlushAndExit()
    {
        // The real core's writer thread must flush its queued stdout before main returns. If the
        // shell stopped reading stdout the moment it retired the child, an 8 MB tail would fill the
        // pipe, the write would never complete, and the core would hang until killed.
        var result = await RunAsync("""
            const chunk = JSON.stringify({type:'noise', pad:'x'.repeat(1000)}) + '\n';
            process.stdin.resume();
            process.stdin.on('end', () => {
              fs.appendFileSync(process.env.COREVIDEO_EXIT_TRACE, 'eof\n');
              let i = 0;
              const pump = () => {
                while (i < 8000) {
                  i++;
                  if (!process.stdout.write(chunk)) { process.stdout.once('drain', pump); return; }
                }
                process.stdout.write('', () => { fs.appendFileSync(process.env.COREVIDEO_EXIT_TRACE, 'flushed\n'); process.exit(0); });
              };
              pump();
            });
            """, supervisor => supervisor.StopForAppExit(Generous));

        Assert.Equal(MediaCoreExitOutcome.ExitedOnItsOwn, result.Outcome);
        Assert.Contains("flushed", result.Trace);
        Assert.Contains("exit:0", result.Trace);
    }

    [Fact]
    public async Task PlainStopStillKillsImmediately()
    {
        // Leave-meeting / respawn keep the immediate kill: a child that would exit on EOF after a
        // long delay must be KILLED, not waited out.
        var result = await RunAsync("""
            process.stdin.resume();
            process.stdin.on('end', () => setTimeout(() => process.exit(0), 60000));
            setInterval(() => {}, 1000);
            """, supervisor =>
            {
                supervisor.Stop();
                return MediaCoreExitOutcome.Killed;
            });

        Assert.DoesNotContain("exit:", result.Trace);
    }

    [Fact]
    public async Task ADescendantThatOutlivesACleanExitIsSwept()
    {
        // After a clean exit, Kill(entireProcessTree) cannot reach the core's children any more.
        // The supervisor snapshots them before closing stdin and kills any survivor.
        int? grandchildPid = null;
        var result = await RunAsync("""
            // detached: libuv otherwise puts the child in a kill-on-close job, which is exactly
            // the protection an unverified real-core child may lack.
            const { spawn } = require('node:child_process');
            const grandchild = spawn(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], { stdio: 'ignore', detached: true, windowsHide: true });
            fs.appendFileSync(process.env.COREVIDEO_EXIT_TRACE, 'grandchild:' + grandchild.pid + '\n');
            process.stdin.resume();
            process.stdin.on('end', () => process.exit(0));
            """,
            supervisor => supervisor.StopForAppExit(Generous),
            beforeStop: async trace =>
            {
                var text = await WaitForTraceAsync(trace, "grandchild:");
                var line = text.Split('\n').First(l => l.StartsWith("grandchild:", StringComparison.Ordinal));
                grandchildPid = int.Parse(line["grandchild:".Length..].Trim());
                using var alive = Process.GetProcessById(grandchildPid.Value);
                Assert.False(alive.HasExited);
            });

        Assert.Equal(MediaCoreExitOutcome.ExitedOnItsOwn, result.Outcome);
        Assert.Contains("exit:0", result.Trace);
        Assert.NotNull(grandchildPid);
        var gone = await IsGoneAsync(grandchildPid.Value);
        if (!gone)
        {
            // Never leave the survivor behind: it holds inherited pipe handles that keep the test
            // host from exiting.
            try { using var survivor = Process.GetProcessById(grandchildPid.Value); survivor.Kill(); } catch { }
        }

        Assert.True(gone, $"grandchild pid {grandchildPid} outlived the clean exit");
    }

    [Fact]
    public async Task APlainStopDuringTheExitGraceKillsTheRetiringCore()
    {
        // The shell's shutdown-timeout fallback calls Stop() while StopForAppExit is still inside
        // its grace. The retiring core must be killed, not orphaned.
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-retiring-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "core.cjs");
            var trace = Path.Combine(directory, "trace.txt");
            await File.WriteAllTextAsync(script,
                "const fs = require('node:fs');\n" +
                "process.on('exit', code => fs.appendFileSync(process.env.COREVIDEO_EXIT_TRACE, 'exit:' + code + '\\n'));\n" +
                Handshake + "\n" +
                "process.stdin.resume();\n" +
                "process.stdin.on('end', () => fs.appendFileSync(process.env.COREVIDEO_EXIT_TRACE, 'eof-ignored\\n'));\n" +
                "setInterval(() => {}, 1000);\n");
            await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                Command = "node", Args = [script], WorkingDirectory = Path.GetTempPath(),
                Environment = new Dictionary<string, string> { ["COREVIDEO_EXIT_TRACE"] = trace },
                HandshakeRequestTimeoutMs = 30000, RequestTimeoutMs = 30000, FrameDrainIntervalMs = 100000, MaxRestarts = 0
            });
            await supervisor.StartAsync().WaitAsync(Generous);
            var process = (Process)typeof(MediaCoreSupervisor)
                .GetField("_process", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(supervisor)!;
            using var child = Process.GetProcessById(process.Id);

            // A grace far longer than the hang guard: only the retiring kill can end this stop.
            var appExit = Task.Run(() => supervisor.StopForAppExit(TimeSpan.FromMinutes(10)));
            await WaitForTraceAsync(trace, "eof-ignored");
            await Task.Run(supervisor.Stop).WaitAsync(HangGuard);

            await child.WaitForExitAsync().WaitAsync(HangGuard);
            // Fix round 2 (N4): the grace thread reports the kill, never "exited on its own".
            Assert.Equal(MediaCoreExitOutcome.Killed, await appExit.WaitAsync(HangGuard));
            Assert.DoesNotContain("exit:", (await TryReadTraceAsync(trace)) ?? string.Empty);
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); } catch { /* best effort */ }
        }
    }

    private static async Task<bool> IsGoneAsync(int pid)
    {
        try
        {
            using var process = Process.GetProcessById(pid);
            await process.WaitForExitAsync().WaitAsync(HangGuard);
            return true;
        }
        catch (ArgumentException)
        {
            return true; // no such process
        }
        catch (TimeoutException)
        {
            return false;
        }
    }
}
