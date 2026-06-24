using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class AudioCaptureDeviceDiscoveryServiceTests
{
    [Fact]
    public void SortForOperatorSelection_PrefersLoopbackBeforePhysicalInputForLocalMachineAudio()
    {
        var sorted = AudioCaptureDeviceDiscoveryService.SortForOperatorSelection(
            [
                Device("asio", "asio-input", "Focusrite USB ASIO"),
                Device("mic", "wasapi-input", "USB microphone"),
                Device("embedded", "embedded-capture-audio", "DeckLink embedded audio"),
                Device("loopback", "wasapi-loopback", "Studio monitor output")
            ]);

        Assert.Equal("loopback", sorted[0].Id);
        Assert.Equal("mic", sorted[1].Id);
        Assert.Equal("embedded", sorted[2].Id);
        Assert.Equal("asio", sorted[3].Id);
    }

    [Fact]
    public void SortForOperatorSelection_PutsDefaultLoopbackBeforeAlphabeticalLoopbacks()
    {
        var sorted = AudioCaptureDeviceDiscoveryService.SortForOperatorSelection(
            [
                Device("chat", "wasapi-loopback", "Chat"),
                Device("system", "wasapi-loopback", "System", isDefault: true),
                Device("mic", "wasapi-input", "USB microphone")
            ]);

        Assert.Equal("system", sorted[0].Id);
        Assert.Equal("chat", sorted[1].Id);
    }

    [Fact]
    public void ResolveDefaultLocalMachineAudioDeviceId_PrefersLoopbackOverMicrophone()
    {
        var selected = AudioCaptureDeviceDiscoveryService.ResolveDefaultLocalMachineAudioDeviceId(
            [
                Device("mic", "wasapi-input", "USB microphone"),
                Device("loopback", "wasapi-loopback", "Studio monitor output")
            ]);

        Assert.Equal("loopback", selected);
    }

    [Fact]
    public void ResolveDefaultLocalMachineAudioDeviceId_PrefersDefaultRenderLoopbackOverAlphabeticalLoopback()
    {
        var selected = AudioCaptureDeviceDiscoveryService.ResolveDefaultLocalMachineAudioDeviceId(
            [
                Device("chat", "wasapi-loopback", "Chat"),
                Device("music", "wasapi-loopback", "Music", isDefault: true),
                Device("system", "wasapi-loopback", "System")
            ]);

        Assert.Equal("music", selected);
    }

    [Fact]
    public void ResolveDefaultLocalMachineAudioDeviceId_FallsBackToFirstUsableDeviceWhenNoLoopbackExists()
    {
        var selected = AudioCaptureDeviceDiscoveryService.ResolveDefaultLocalMachineAudioDeviceId(
            [
                Device("asio", "asio-input", "Focusrite USB ASIO"),
                Device("mic", "wasapi-input", "USB microphone")
            ]);

        Assert.Equal("mic", selected);
    }

    [Theory]
    [InlineData("wasapi-loopback", true)]
    [InlineData("loopback", true)]
    [InlineData("wasapi-render-loopback", true)]
    [InlineData("system-loopback", true)]
    [InlineData("wasapi-input", false)]
    public void IsLoopbackSource_IdentifiesRenderEndpointCaptureKinds(string sourceKind, bool expected)
    {
        Assert.Equal(expected, AudioCaptureDeviceDiscoveryService.IsLoopbackSource(Device("device", sourceKind, "Device")));
    }

    [Theory]
    [InlineData("wasapi-loopback", 0)]
    [InlineData("loopback", 0)]
    [InlineData("wasapi-input", 1)]
    [InlineData("wasapi-capture", 1)]
    [InlineData("embedded-capture-audio", 2)]
    [InlineData("asio-input", 3)]
    [InlineData("unknown", 4)]
    public void SourceKindPriority_RanksLocalAudioKindsByOperatorUsefulness(string sourceKind, int expected)
    {
        Assert.Equal(expected, AudioCaptureDeviceDiscoveryService.SourceKindPriority(sourceKind));
    }

    private static AudioCaptureDevice Device(string id, string sourceKind, string name, bool isDefault = false) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native:{id}",
            Name = name,
            SourceKind = sourceKind,
            DriverName = sourceKind,
            IsDefault = isDefault
        };
}
