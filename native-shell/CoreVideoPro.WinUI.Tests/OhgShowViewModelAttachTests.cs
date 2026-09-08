using System.Threading;
using System.Threading.Channels;
using CoreVideoPro.Control;
using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Final-review guard for <see cref="OhgShowViewModel.Attach"/> (Plan 7b, M7).
///
/// <para><b>The gap.</b> The view model applies whatever the bridge already holds at CONSTRUCTION,
/// and then only sees the future through the bridge's events. Attach happens later (the page's
/// Loaded handler), so a snapshot published in between reached neither path: the tab sat on an empty
/// board and a stale "stopped" until the engine's next publish, which on an idle show can be a long
/// time. Attach now re-reads <c>Latest</c> and <c>Health</c> AFTER subscribing — subscribe-then-read,
/// so the worst case is applying one revision twice (the envelope gate swallows it) rather than
/// losing one.</para>
///
/// <para>Driven over a REAL <see cref="ShowEngineSupervisor"/> + <see cref="ShowEngineBridge"/> with
/// a scripted child, because <c>Attach</c> takes the concrete bridge — mocking it would prove
/// nothing about the property it actually reads.</para>
/// </summary>
public sealed class OhgShowViewModelAttachTests
{
    [Fact]
    public async Task Attach_AppliesASnapshotPublishedBeforeTheViewModelSubscribed()
    {
        var factory = new ScriptedChildFactory();
        using var supervisor = NewSupervisor(factory);
        using var bridge = new ShowEngineBridge(supervisor, OscExposure.LoopbackOnly);

        await supervisor.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => supervisor.Health.State == ShowEngineState.Running, "the engine to be running");

        // Published BEFORE the view model exists — the exact race Attach now closes.
        factory.Child.Push(SnapshotLine(generation: 1, revision: 12));
        await WaitUntil(() => bridge.Latest?.Revision == 12, "the bridge to hold revision 12");

        var vm = new OhgShowViewModel(
            new FakeOhgActionInvoker(),
            action => action(),
            () => null,          // nothing to seed at construction: the snapshot arrived after it
            () => new ShowEngineHealth(ShowEngineState.Stopped, 0, 0, null, null),
            Array.Empty<OhgLookOption>(),
            driveHost: true);

        Assert.Equal(0L, vm.Revision);
        Assert.Equal("stopped", vm.EngineState);

        vm.Attach(bridge);

        Assert.Equal(12L, vm.Revision);
        Assert.Equal("running", vm.EngineState);

        vm.Dispose();
    }

    /// <summary>Attaching to a bridge with nothing published yet must be a quiet no-op, not a throw
    /// and not a fabricated revision.</summary>
    [Fact]
    public void Attach_WithNoSnapshotYet_LeavesTheBoardEmpty()
    {
        var factory = new ScriptedChildFactory();
        using var supervisor = NewSupervisor(factory);
        using var bridge = new ShowEngineBridge(supervisor, OscExposure.LoopbackOnly);

        var vm = new OhgShowViewModel(
            new FakeOhgActionInvoker(),
            action => action(),
            () => null,
            () => new ShowEngineHealth(ShowEngineState.Stopped, 0, 0, null, null),
            Array.Empty<OhgLookOption>(),
            driveHost: true);

        vm.Attach(bridge);

        Assert.Equal(0L, vm.Revision);
        Assert.Empty(vm.Slots);
        Assert.Equal("stopped", vm.EngineState);

        vm.Dispose();
    }

    // ── rig ──────────────────────────────────────────────────────────────────────────

    private static readonly ShowEngineSpawnRequest Request = new(
        NodeExe: @"C:\node\node.exe",
        EntryScript: @"C:\app\show-engine\dist\host\main.js",
        ConfigPath: @"C:\cfg\ohg-show-config.json",
        WorkingDirectory: @"C:\app\show-engine",
        Environment: new Dictionary<string, string>());

    private static ShowEngineSupervisor NewSupervisor(ScriptedChildFactory factory) =>
        new(factory,
            new ShowEngineRestartPolicy(),
            // A REAL delay: the supervisor races the preloaded handshake against this wait, and an
            // instantly-completed delay makes it give up before the reader has parsed the handshake.
            (d, ct) => Task.Delay(d, ct),
            () => new DateTimeOffset(2026, 9, 7, 12, 0, 0, TimeSpan.Zero),
            new ShowEngineSupervisorOptions());

    private static async Task WaitUntil(Func<bool> condition, string what, int timeoutMs = 5000)
    {
        var deadline = Environment.TickCount64 + timeoutMs;
        while (!condition())
        {
            if (Environment.TickCount64 > deadline) Assert.Fail("timed out waiting for " + what);
            await Task.Delay(1).ConfigureAwait(false);
        }
    }

    private static string HandshakeLine(int generation) =>
        "{\"event\":\"handshake\",\"protocolVersion\":1,\"engineVersion\":\"0.1.0\",\"generation\":" + generation +
        ",\"actions\":[{\"id\":\"ohg.program.cut\",\"title\":\"Cut\",\"description\":\"Cut\",\"params\":[]}]," +
        "\"fieldTemplates\":[\"ohg/look\"],\"snapshot\":{\"revision\":3},\"fields\":{}}";

    private static string SnapshotLine(int generation, long revision) =>
        "{\"event\":\"snapshot\",\"generation\":" + generation + ",\"revision\":" + revision +
        ",\"snapshot\":{\"revision\":" + revision + "},\"fields\":{}}";

    /// <summary>The smallest child that satisfies the supervisor's reader loop: a line queue in, a
    /// never-completing exit task, and writes swallowed (nothing in these tests awaits a response).</summary>
    private sealed class ScriptedChild : IShowEngineChild
    {
        private readonly Channel<string> _lines = Channel.CreateUnbounded<string>();
        private readonly TaskCompletionSource<int> _exited = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ScriptedChild(int generation) => Generation = generation;

        public int Generation { get; }

        public Task<int> Exited => _exited.Task;

        public void Push(string line) => _lines.Writer.TryWrite(line);

        public Task WriteLineAsync(string line, CancellationToken ct) => Task.CompletedTask;

        public async Task<string?> ReadLineAsync(CancellationToken ct)
        {
            try
            {
                return await _lines.Reader.ReadAsync(ct).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return null;
            }
            catch (ChannelClosedException)
            {
                return null;
            }
        }

        public void Kill()
        {
            _lines.Writer.TryComplete();
            _exited.TrySetResult(0);
        }

        public void Dispose() => Kill();
    }

    private sealed class ScriptedChildFactory : IShowEngineChildFactory
    {
        public ScriptedChild Child { get; private set; } = null!;

        public IShowEngineChild Spawn(int generation, ShowEngineSpawnRequest request)
        {
            var child = new ScriptedChild(generation);
            child.Push(HandshakeLine(generation));
            Child = child;
            return child;
        }
    }
}
