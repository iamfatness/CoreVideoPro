using CommunityToolkit.Mvvm.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Models;

public sealed class SceneListItem
{
    public required Scene Scene { get; init; }

    public bool IsProgram { get; init; }

    public bool IsPreview { get; init; }

    public string StatusLabel =>
        IsProgram ? "Program" : IsPreview ? "Preview" : string.Empty;

    public bool HasStatusLabel => StatusLabel.Length > 0;

    public Brush ItemBorderBrush { get; init; } = new SolidColorBrush(Windows.UI.Color.FromArgb(26, 189, 207, 196));

    public Brush ItemBackgroundBrush { get; init; } = new SolidColorBrush(Windows.UI.Color.FromArgb(138, 10, 13, 12));

    public Microsoft.UI.Xaml.Thickness ItemBorderThickness { get; init; } = new(1);

    public Microsoft.UI.Xaml.Thickness ProgramAccentMargin { get; init; } = new(0);
}

public sealed class ParticipantListItem
{
    public Participant Participant { get; init; } = new();

    public IRelayCommand<string>? SelectCommand { get; init; }

    public bool IsSelected { get; init; }

    public bool IsTalking { get; init; }

    public Brush BorderBrush { get; init; } = new SolidColorBrush(Windows.UI.Color.FromArgb(26, 189, 207, 196));

    public Microsoft.UI.Xaml.Thickness BorderThickness { get; init; } = new(1);

    public bool ShowTalkingIndicator => IsTalking;

    public Visibility TalkingIndicatorVisibility =>
        IsTalking ? Visibility.Visible : Visibility.Collapsed;
}

public static class StudioHighlightBrushes
{
    private static readonly SolidColorBrush DefaultBorder =
        new(Windows.UI.Color.FromArgb(26, 189, 207, 196));

    private static readonly SolidColorBrush PreviewBorder =
        new(Windows.UI.Color.FromArgb(204, 68, 193, 161));

    private static readonly SolidColorBrush PreviewBackground =
        new(Windows.UI.Color.FromArgb(31, 68, 193, 161));

    private static readonly SolidColorBrush ProgramBackground =
        new(Windows.UI.Color.FromArgb(138, 10, 13, 12));

    private static readonly SolidColorBrush ProgramAccent =
        new(Windows.UI.Color.FromArgb(255, 240, 168, 92));

    private static readonly SolidColorBrush SelectedParticipantBorder =
        new(Windows.UI.Color.FromArgb(140, 61, 220, 151));

    private static readonly SolidColorBrush TalkingParticipantBorder =
        new(Windows.UI.Color.FromArgb(217, 127, 217, 160));

    public static SceneListItem ForScene(Scene scene, string activeSceneId, string previewSceneId)
    {
        var isProgram = scene.Id == activeSceneId;
        var isPreview = scene.Id == previewSceneId;

        return new SceneListItem
        {
            Scene = scene,
            IsProgram = isProgram,
            IsPreview = isPreview,
            ItemBorderBrush = isPreview ? PreviewBorder : DefaultBorder,
            ItemBackgroundBrush = isPreview ? PreviewBackground : ProgramBackground,
            ItemBorderThickness = isPreview ? new Thickness(1) : new Thickness(1),
            ProgramAccentMargin = isProgram ? new Thickness(3, 0, 0, 0) : new Thickness(0)
        };
    }

    public static ParticipantListItem ForParticipant(
        Participant participant,
        string? selectedParticipantId,
        IRelayCommand<string>? selectCommand = null)
    {
        var isSelected = participant.Id == selectedParticipantId;
        var isTalking = participant.IsActiveSpeaker;

        Brush borderBrush = DefaultBorder;
        Thickness borderThickness = new(1);

        if (isSelected)
        {
            borderBrush = SelectedParticipantBorder;
            borderThickness = new Thickness(2);
        }
        else if (isTalking)
        {
            borderBrush = TalkingParticipantBorder;
            borderThickness = new Thickness(2);
        }

        return new ParticipantListItem
        {
            Participant = participant,
            SelectCommand = selectCommand,
            IsSelected = isSelected,
            IsTalking = isTalking,
            BorderBrush = borderBrush,
            BorderThickness = borderThickness
        };
    }

    public static SolidColorBrush ProgramAccentBar => ProgramAccent;
}