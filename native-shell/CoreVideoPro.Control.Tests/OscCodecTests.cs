using CoreVideoPro.Control.Osc;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class OscCodecTests
{
    [Fact]
    public void RoundTrips_MixedArgumentTypes()
    {
        var message = new OscMessage("/cvp/input/name", 3, "Dr. Jane Smith", true, false, 0.75f);

        var bytes = OscCodec.EncodeMessage(message);
        // OSC packets are always a multiple of 4 bytes.
        Assert.Equal(0, bytes.Length % 4);

        var decoded = Assert.Single(OscCodec.Decode(bytes));
        Assert.Equal("/cvp/input/name", decoded.Address);
        Assert.Equal(5, decoded.Args.Count);
        Assert.Equal(3, Assert.IsType<int>(decoded.Args[0]));
        Assert.Equal("Dr. Jane Smith", Assert.IsType<string>(decoded.Args[1]));
        Assert.True(Assert.IsType<bool>(decoded.Args[2]));
        Assert.False(Assert.IsType<bool>(decoded.Args[3]));
        Assert.Equal(0.75f, Assert.IsType<float>(decoded.Args[4]));
    }

    [Theory]
    [InlineData("/a", "x")]        // 2-byte address, 1-byte string
    [InlineData("/abc", "abcd")]   // 4-byte string (needs a full pad word)
    [InlineData("/cvp/take", "")]  // empty string arg
    public void StringPadding_AlignsToFourBytes(string address, string arg)
    {
        var bytes = OscCodec.EncodeMessage(new OscMessage(address, arg));
        Assert.Equal(0, bytes.Length % 4);
        var decoded = Assert.Single(OscCodec.Decode(bytes));
        Assert.Equal(address, decoded.Address);
        Assert.Equal(arg, decoded.Args[0]);
    }

    [Fact]
    public void AddressOnly_DecodesWithNoArgs()
    {
        var bytes = OscCodec.EncodeMessage(new OscMessage("/cvp/transport/take"));
        var decoded = Assert.Single(OscCodec.Decode(bytes));
        Assert.Equal("/cvp/transport/take", decoded.Address);
        Assert.Empty(decoded.Args);
    }

    [Fact]
    public void Decodes_Bundle_IntoComponentMessages()
    {
        // Build a #bundle: "#bundle\0" + timetag(8) + [size + element]*
        var a = OscCodec.EncodeMessage(new OscMessage("/cvp/transport/take"));
        var b = OscCodec.EncodeMessage(new OscMessage("/cvp/scene/select", "interview"));
        var bundle = new List<byte>();
        bundle.AddRange(System.Text.Encoding.ASCII.GetBytes("#bundle\0"));
        bundle.AddRange(new byte[8]);  // immediate timetag
        void Add(byte[] elem)
        {
            var size = new byte[4];
            System.Buffers.Binary.BinaryPrimitives.WriteInt32BigEndian(size, elem.Length);
            bundle.AddRange(size);
            bundle.AddRange(elem);
        }
        Add(a);
        Add(b);

        var messages = OscCodec.Decode(bundle.ToArray());
        Assert.Equal(2, messages.Count);
        Assert.Equal("/cvp/transport/take", messages[0].Address);
        Assert.Equal("/cvp/scene/select", messages[1].Address);
        Assert.Equal("interview", messages[1].Args[0]);
    }

    [Fact]
    public void MalformedPacket_DecodesToEmpty_NeverThrows()
    {
        Assert.Empty(OscCodec.Decode(new byte[] { 1, 2, 3 }));              // no leading slash
        Assert.Empty(OscCodec.Decode(System.Array.Empty<byte>()));         // empty
        Assert.Empty(OscCodec.Decode(new byte[] { (byte)'/', (byte)'a' })); // unterminated address
    }
}
