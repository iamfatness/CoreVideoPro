namespace CoreVideoPro.WinUI.Services;

/// <summary>A swap-chain readiness signal remains owned until a present commits.</summary>
internal sealed class PresentationCredit
{
    internal bool Held { get; private set; }
    internal bool TryAcquire(Func<bool> poll) => Held || (Held = poll());
    internal void Commit() => Held = false;
    internal void Reset() => Held = false;
}
