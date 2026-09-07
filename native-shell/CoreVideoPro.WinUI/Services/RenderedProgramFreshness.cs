using CoreVideoPro.MediaCore.Models;
namespace CoreVideoPro.WinUI.Services;

public sealed class RenderedProgramFreshness
{
    private string? _intent;
    private bool _awaitingAck;
    private int _ackFrame;
    public long Revision { get; private set; }
    public void Observe(string intent)
    {
        if (_intent == intent) return;
        _intent = intent;
        Revision++;
        _awaitingAck = true;
    }
    public void Acknowledge(long revision, int frameNumber)
    {
        if (revision != Revision || !_awaitingAck) return;
        _ackFrame = frameNumber;
        _awaitingAck = false;
    }
    public bool Accepts(NativeMediaCoreProgramFrame? frame) =>
        !_awaitingAck && frame is not null && frame.FrameNumber > _ackFrame;
}
