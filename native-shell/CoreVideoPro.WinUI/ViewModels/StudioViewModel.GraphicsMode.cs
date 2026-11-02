using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>Graphics tab mode and the Studio-side graphics chrome (issue #1076, Stage E).
/// <see cref="GraphicsMode"/> is a UI preference persisted with the other operator UI
/// preferences (production output preferences); it never changes Program or Preview.</summary>
public sealed partial class StudioViewModel
{
    public const string GraphicsModePlayout = "playout", GraphicsModeDesign = "design";
    private string _graphicsMode = GraphicsModePlayout;
    private bool _graphicsFooterBound;

    /// <summary>"playout" (default) or "design". Any other value normalises to playout.</summary>
    public string GraphicsMode
    {
        get => _graphicsMode;
        set { if (ApplyGraphicsMode(value)) SaveProductionOutputPreferences(); }
    }
    public bool IsGraphicsPlayoutMode => _graphicsMode == GraphicsModePlayout;
    public bool IsGraphicsDesignMode => _graphicsMode == GraphicsModeDesign;

    /// <summary>Restore from saved preferences without writing them back.</summary>
    internal void RestoreGraphicsMode(string? mode) => ApplyGraphicsMode(mode);

    private bool ApplyGraphicsMode(string? value)
    {
        var next = value == GraphicsModeDesign ? GraphicsModeDesign : GraphicsModePlayout;
        if (!SetProperty(ref _graphicsMode, next, nameof(GraphicsMode))) return false;
        OnPropertyChanged(nameof(IsGraphicsPlayoutMode)); OnPropertyChanged(nameof(IsGraphicsDesignMode));
        NotifyGraphicsDesignSurface();
        return true;
    }

    /// <summary>The compact playlist column in the Studio tab (spec: right panel of the console).</summary>
    [ObservableProperty] private bool _showGraphicsPanel = true;

    /// <summary>Design mode shows the template-first builder by default; the full workspace
    /// (looks library, private cues, layer editor, automation) sits behind "Advanced editor".
    /// Session-only: a new launch always opens on the builder.</summary>
    [ObservableProperty] private bool _graphicsDesignAdvanced;
    /// <summary>Playlist row the builder opens for editing ("" = start a new graphic). Set by
    /// Operate's "Open in Design"; the builder clears it after loading the row.</summary>
    [ObservableProperty] private string _graphicsDesignRowId = "";
    /// <summary>Graphics Designer slice 2: a design is open in the Designer (layers, canvas, inspector). It wins over
    /// the builder and the advanced workspace while Design mode is showing. Session-only.</summary>
    [ObservableProperty] private bool _graphicsDesignerOpen;
    public GraphicsDesignSurface GraphicsDesignSurface => GraphicsDesignSurfaces.For(_graphicsMode, GraphicsDesignerOpen, GraphicsDesignAdvanced);
    public bool IsGraphicsBuilderVisible => GraphicsDesignSurface == GraphicsDesignSurface.Builder;
    public bool IsGraphicsAdvancedVisible => GraphicsDesignSurface == GraphicsDesignSurface.Workspace;
    public bool IsGraphicsDesignerVisible => GraphicsDesignSurface == GraphicsDesignSurface.Designer;
    partial void OnGraphicsDesignAdvancedChanged(bool value) => NotifyGraphicsDesignSurface();
    partial void OnGraphicsDesignerOpenChanged(bool value) => NotifyGraphicsDesignSurface();
    private void NotifyGraphicsDesignSurface()
    {
        OnPropertyChanged(nameof(GraphicsDesignSurface)); OnPropertyChanged(nameof(IsGraphicsBuilderVisible));
        OnPropertyChanged(nameof(IsGraphicsAdvancedVisible)); OnPropertyChanged(nameof(IsGraphicsDesignerVisible));
    }

    /// <summary>Operate → Design for one row: the builder opens that row's look and fields.
    /// Program and Preview are unchanged.</summary>
    public void OpenGraphicInDesign(string rowId)
    {
        GraphicsDesignRowId = rowId; GraphicsDesignAdvanced = false; GraphicsDesignerOpen = false;
        GraphicsMode = GraphicsModeDesign; ActiveTab = StudioTab.Overlays;
    }

    /// <summary>Canonical source id of the source currently occupying Program (the Guest-row
    /// subject a bare hotkey or "LT in" plays), or null when Program shows no eligible source.</summary>
    public string? GraphicsProgramSubjectSourceId => ResolveProgramLowerThirdSource(ProgramSceneRoutes)?.SourceId;

    /// <summary>True when a playlist Guest-row instance is on Program per native strip truth.</summary>
    public bool GraphicsStripLowerThirdOnAir =>
        _graphicsPlayout is { } playout && playout.Strip.OnProgram.Any(i => i.RowId.Length > 0 && playout.Row(i.RowId)?.Kind == GraphicsPlaylistRow.Guest);

    /// <summary>Footer / header LT button text: the strip's lower-third chip wins when a playlist
    /// lower third is on air; otherwise the legacy key label as before.</summary>
    public string GraphicsFooterLowerThirdLabel { get { EnsureGraphicsFooterBound(); return GraphicsStripLowerThirdOnAir ? FormatStudioLowerThirdActionLabel(true) : StudioLowerThirdCompactLabel; } }
    public string GraphicsFooterLowerThirdStatus { get { EnsureGraphicsFooterBound(); return GraphicsStripLowerThirdOnAir ? "On air" : StudioLowerThirdCompactStatus; } }

    private void EnsureGraphicsFooterBound()
    {
        if (_graphicsFooterBound) return;
        _graphicsFooterBound = true;
        _ = GraphicsPlayout; // strip truth needs the service observing native state
        PropertyChanged += OnGraphicsFooterSourceChanged;
    }

    private void OnGraphicsFooterSourceChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(GraphicsStrip) or nameof(StudioLowerThirdCompactLabel) or nameof(StudioLowerThirdCompactStatus)) {
            OnPropertyChanged(nameof(GraphicsStripLowerThirdOnAir));
            OnPropertyChanged(nameof(GraphicsFooterLowerThirdLabel));
            OnPropertyChanged(nameof(GraphicsFooterLowerThirdStatus));
        }
    }
}
