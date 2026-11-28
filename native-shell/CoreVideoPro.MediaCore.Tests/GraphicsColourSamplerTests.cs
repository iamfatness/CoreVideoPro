using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>Graphics styles slice 3: the eyedropper freezes the core's Program thumbnail and reads a
/// pixel of that frozen copy. The answer comes only from the snapshot that answered the command.</summary>
public sealed class GraphicsColourSamplerTests
{
    private static readonly JsonSerializerOptions Web = new(JsonSerializerDefaults.Web);
    private readonly List<JsonElement> _sent = [];
    private NativeColourSampleState? _node;
    private Func<JsonElement, NativeColourSampleState?>? _answer;

    private Task<NativeMediaCoreStateSnapshot> Send(IReadOnlyList<NativeMediaCoreCommand> commands, CancellationToken _)
    {
        var command = JsonSerializer.SerializeToElement(commands.Single(), Web);
        _sent.Add(command);
        if (_answer is not null) _node = _answer(command);
        return Task.FromResult(new NativeMediaCoreStateSnapshot { ColourSample = _node });
    }
    private GraphicsColourSampler Sampler(bool running = true) => new(() => running, Send);
    private static NativeColourSampleState Frozen(long frame = 12) => new() {
        State = "frozen", FrameNumber = frame, Width = 2, Height = 1, Source = "program",
        BgraBase64 = Convert.ToBase64String([0, 0, 255, 255, 255, 0, 0, 255]) };

    [Fact]
    public async Task FreezeReturnsExactlyTheFrozenBytesAndSendsAProgramFreeze()
    {
        _answer = _ => Frozen();
        var (frame, reason) = await Sampler().FreezeAsync();
        Assert.NotNull(frame);
        Assert.Equal(12, frame!.FrameNumber);
        Assert.Equal(new byte[] { 0, 0, 255, 255, 255, 0, 0, 255 }, frame.Bgra);
        Assert.Contains("frozen", reason);
        var sent = Assert.Single(_sent);
        Assert.Equal("colour-sample", sent.GetProperty("type").GetString());
        Assert.Equal("freeze", sent.GetProperty("action").GetString());
        Assert.Equal("program", sent.GetProperty("source").GetString());
    }

    [Fact]
    public async Task PickReturnsTheCoreAnswerOnlyWhenItMatchesTheRequestAndFrame()
    {
        _answer = _ => Frozen();
        var (frame, _) = await Sampler().FreezeAsync();
        _answer = command => Frozen() with { LastPick = new() { RequestId = command.GetProperty("requestId").GetString()!, FrameNumber = 12, X = 1, Y = 0, Hex = "#0000FF" } };
        var (hex, reason) = await Sampler().PickAsync(frame!, 0.75, 0.5);
        Assert.Equal("#0000FF", hex);
        Assert.Contains("1, 0", reason);
        var pick = _sent.Last();
        Assert.Equal(0.75, pick.GetProperty("x").GetDouble());
        Assert.Equal(12, pick.GetProperty("frameNumber").GetInt64());
        // A pick for another request (a stale answer) is never shown as this pick's colour.
        _answer = _ => Frozen() with { LastPick = new() { RequestId = "someone-else", FrameNumber = 12, Hex = "#FF0000" } };
        Assert.Null((await Sampler().PickAsync(frame!, 0.1, 0.1)).Hex);
        _answer = _ => Frozen() with { LastError = "colour-sample-frame-changed" };
        var refused = await Sampler().PickAsync(frame!, 0.1, 0.1);
        Assert.Null(refused.Hex);
        Assert.Contains("Freeze Program again", refused.Reason);
    }

    [Fact]
    public async Task HonestRefusalsWithoutACoreOrAFrame()
    {
        Assert.Null((await Sampler(running: false).FreezeAsync()).Frame);
        Assert.Empty(_sent);
        _answer = _ => null;
        Assert.Contains("cannot sample", (await Sampler().FreezeAsync()).Reason);
        _answer = _ => new NativeColourSampleState { LastError = "colour-sample-no-frame" };
        Assert.Contains("no thumbnail", (await Sampler().FreezeAsync()).Reason);
        _answer = _ => Frozen() with { Width = 3 }; // byte count disagrees with the declared size
        Assert.Null((await Sampler().FreezeAsync()).Frame);
    }

    [Fact]
    public void TheCoreNodeDeserializesFromItsWireShape()
    {
        const string wire = """{"state":"frozen","revision":3,"lastPick":{"requestId":"pick-a","frameNumber":41,"x":3,"y":1,"r":255,"g":255,"b":255,"hex":"#FFFFFF"},"lastError":"","source":"program","frameNumber":41,"width":4,"height":2,"pixelFormat":"bgra","bgraBase64":"AAAA"}""";
        var node = JsonSerializer.Deserialize<NativeColourSampleState>(wire, Web)!;
        Assert.Equal("frozen", node.State);
        Assert.Equal(41, node.FrameNumber);
        Assert.Equal("#FFFFFF", node.LastPick!.Hex);
        Assert.Equal(3, node.LastPick.X);
        var idle = JsonSerializer.Deserialize<NativeColourSampleState>("""{"state":"idle","revision":0,"lastPick":null,"lastError":""}""", Web)!;
        Assert.Null(idle.LastPick);
    }
}
