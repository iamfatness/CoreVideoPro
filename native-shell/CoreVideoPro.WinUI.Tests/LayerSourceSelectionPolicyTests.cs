using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// #480: ComboBox ItemsSource rebuilds fire SelectionChanged with the blank
/// placeholder. That is not an operator pick.
/// </summary>
public sealed class LayerSourceSelectionPolicyTests
{
    [Fact]
    public void AComboBoxRefreshThatLandsOnBlankIsNotAnOperatorPick()
    {
        Assert.False(LayerSourceSelectionPolicy.ShouldCommit(operatorGesture: false, incomingValue: ""));
        Assert.False(LayerSourceSelectionPolicy.ShouldCommit(operatorGesture: false, incomingValue: "input-01"));
    }

    [Fact]
    public void AnOperatorCanClearALayerToBlank()
        => Assert.True(LayerSourceSelectionPolicy.ShouldCommit(operatorGesture: true, incomingValue: ""));

    [Fact]
    public void AnOperatorPickOfASourceCommits()
        => Assert.True(LayerSourceSelectionPolicy.ShouldCommit(operatorGesture: true, incomingValue: "input-01"));

    [Fact]
    public void ANullIncomingValueNeverCommits()
        => Assert.False(LayerSourceSelectionPolicy.ShouldCommit(operatorGesture: true, incomingValue: null));
}
