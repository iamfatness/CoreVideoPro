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
/// </summary>
public sealed class MediaCoreAppExitStopTests
{
    private const string Handshake =
        "console.log(JSON.stringify({id:'handshake',ok:true,type:'handshake',protocolVersion:{major:1,minor:0},profile:{name:'fake',renderer:'software',maxProgramResolution:'1920x1080'}}));";

    private static async Task<(MediaCoreExitOutcome Outcome, TimeSpan Elapsed, string Trace)> RunAsync(
        string body, TimeSpan grace)
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
                HandshakeRequestTimeoutMs = 5000, RequestTimeoutMs = 5000, FrameDrainIntervalMs = 100000, MaxRestarts = 0
            });
            await supervisor.StartAsync().WaitAsync(TimeSpan.FromSeconds(10));
            var process = (Process)typeof(MediaCoreSupervisor)
                .GetField("_process", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(supervisor)!;
            using var child = Process.GetProcessById(process.Id);

            var stopwatch = Stopwatch.StartNew();
            var outcome = await Task.Run(() => supervisor.StopForAppExit(grace));
            stopwatch.Stop();
            await child.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(supervisor.Running);
            var traceText = File.Exists(trace) ? await File.ReadAllTextAsync(trace) : string.Empty;
            return (outcome, stopwatch.Elapsed, traceText);
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); } catch { /* a dying child can hold the cwd briefly */ }
        }
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
            """, TimeSpan.FromSeconds(3));

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
            """, TimeSpan.FromMilliseconds(300));

        Assert.Equal(MediaCoreExitOutcome.Killed, result.Outcome);
        Assert.Contains("eof-ignored", result.Trace);
        Assert.DoesNotContain("exit:", result.Trace);
        Assert.True(result.Elapsed >= TimeSpan.FromMilliseconds(250), $"killed before the grace ran out ({result.Elapsed})");
        Assert.True(result.Elapsed < TimeSpan.FromMilliseconds(300 + MediaCoreSupervisor.KillWaitMilliseconds + 2000),
            $"the stop overran its bound ({result.Elapsed})");
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
            """, TimeSpan.FromSeconds(5));

        Assert.Equal(MediaCoreExitOutcome.ExitedOnItsOwn, result.Outcome);
        Assert.Contains("flushed", result.Trace);
        Assert.Contains("exit:0", result.Trace);
    }

    [Fact]
    public async Task PlainStopStillKillsImmediately()
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-plain-stop-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "core.cjs");
            await File.WriteAllTextAsync(script, Handshake + """

                process.stdin.resume();
                process.stdin.on('end', () => setTimeout(() => process.exit(0), 3000));
                setInterval(() => {}, 1000);
                """);
            await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                Command = "node", Args = [script], WorkingDirectory = Path.GetTempPath(),
                HandshakeRequestTimeoutMs = 5000, RequestTimeoutMs = 5000, FrameDrainIntervalMs = 100000, MaxRestarts = 0
            });
            await supervisor.StartAsync().WaitAsync(TimeSpan.FromSeconds(10));
            var process = (Process)typeof(MediaCoreSupervisor)
                .GetField("_process", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(supervisor)!;
            using var child = Process.GetProcessById(process.Id);

            var stopwatch = Stopwatch.StartNew();
            await Task.Run(supervisor.Stop);
            await child.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));

            // Leave-meeting / respawn keep the immediate kill: no 3 s wait for the child's own exit.
            Assert.True(stopwatch.Elapsed < TimeSpan.FromMilliseconds(2500), $"plain Stop waited ({stopwatch.Elapsed})");
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); } catch { /* best effort */ }
        }
    }
}
