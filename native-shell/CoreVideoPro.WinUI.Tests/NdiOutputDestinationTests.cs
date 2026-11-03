using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class NdiOutputDestinationTests
{
    [Fact]
    public void Off_SelectsNothing() =>
        Assert.Empty(StudioStreamOutputValidation.SelectNdiDestinations(false, "Studio A", true, false));

    [Fact]
    public void On_WithName_SelectsProgramOnly() =>
        Assert.Equal(new[] { "ndi" }, StudioStreamOutputValidation.SelectNdiDestinations(true, "Studio A", false, false));

    [Fact]
    public void On_WithKey_SelectsProgramAndKey() =>
        Assert.Equal(new[] { "ndi", "ndi-graphics" }, StudioStreamOutputValidation.SelectNdiDestinations(true, "Studio A", true, false));

    [Fact]
    public void On_BlankName_ValidatedOnly_SelectsNothing() =>
        Assert.Empty(StudioStreamOutputValidation.SelectNdiDestinations(true, " ", false, true));

    [Fact]
    public void On_BlankName_NotValidatedOnly_SelectsProgram() =>
        Assert.Equal(new[] { "ndi" }, StudioStreamOutputValidation.SelectNdiDestinations(true, " ", false, false));
}
