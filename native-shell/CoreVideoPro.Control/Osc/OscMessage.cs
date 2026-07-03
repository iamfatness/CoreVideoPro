namespace CoreVideoPro.Control.Osc;

/// <summary>A decoded/encodable OSC 1.0 message: an address pattern plus positional arguments.
/// Argument runtime types after decode: <see cref="int"/> (i), <see cref="float"/> (f),
/// <see cref="string"/> (s), <see cref="bool"/> (T/F), <see cref="byte"/>[] (b).</summary>
public sealed record OscMessage(string Address, IReadOnlyList<object?> Args)
{
    public OscMessage(string address, params object?[] args) : this(address, (IReadOnlyList<object?>)args)
    {
    }
}
