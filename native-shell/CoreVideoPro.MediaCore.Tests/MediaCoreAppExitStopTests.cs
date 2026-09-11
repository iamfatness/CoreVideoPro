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
            var traceText = File.Exists(trace) ? await File.ReadAllTextAsync(trace) : string.Empty;
            return new Run(outcome, stopwatch.Elapsed, traceText);
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); } catch { /* a dying child can hold the cwd briefly */ }
        }
    }

    private static async Task<string> WaitForTraceAsync(string trace, string marker)
    {
        var deadline = Stopwatch.StartNew();
        while (deadline.Elapsed < HangGuard)
        {
            if (File.Exists(trace))
            {
                var text = await File.ReadAllTextAsync(trace);
                if (text.Contains(marker, StringComparison.Ordinal)) return text;
            }

            await Task.Delay(50);
        }

        throw new TimeoutException($"'{marker}' never appeared in the trace");
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
            Assert.DoesNotContain("exit:", await File.ReadAllTextAsync(trace));
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
