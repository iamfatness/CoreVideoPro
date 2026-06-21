using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class AudioCaptureDeviceDiscoveryServiceTests
{
    [Fact]
    public void SortForOperatorSelection_PrefersPhysicalInputBeforeLoopbackForLocalMachineAudioDefault()
    {
        var sorted = AudioCaptureDeviceDiscoveryService.SortForOperatorSelection(
            [
                Device("asio", "asio-input", "Focusrite USB ASIO"),
                Device("mic", "wasapi-input", "USB microphone"),
                Device("embedded", "embedded-capture-audio", "DeckLink embedded audio"),
                Device("loopback", "wasapi-loopback", "Studio monitor output")
            ]);

        Assert.Equal("mic", sorted[0].Id);
        Assert.Equal("loopback", sorted[1].Id);
        Assert.Equal("embedded", sorted[2].Id);
        Assert.Equal("asio", sorted[3].Id);
    }

    [Theory]
    [InlineData("wasapi-input", 0)]
    [InlineData("wasapi-capture", 0)]
    [InlineData("wasapi-loopback", 1)]
    [InlineData("loopback", 1)]
    [InlineData("embedded-capture-audio", 2)]
    [InlineData("asio-input", 3)]
    [InlineData("unknown", 4)]
    public void SourceKindPriority_RanksLocalAudioKindsByOperatorUsefulness(string sourceKind, int expected)
    {
        Assert.Equal(expected, AudioCaptureDeviceDiscoveryService.SourceKindPriority(sourceKind));
    }

    private static AudioCaptureDevice Device(string id, string sourceKind, string name) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native:{id}",
            Name = name,
            SourceKind = sourceKind,
            DriverName = sourceKind
        };
}
