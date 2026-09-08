using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// GFX/data panel commands and lamps (Plan 7b Task 6) — the headline overlay editor, the question
/// overlay, the Mukana registry override editor, and the three capability lamps. Every command is
/// a thin translation to an <c>ohg.gfx.*</c>/<c>ohg.mukana.*</c> action id via
/// <see cref="OhgActionArgs"/>, invoked through <see cref="_invoker"/>, and reported through
/// <see cref="ReportResult"/> (defined on the core partial, shared with Tasks 4/5). The lamps and
/// labels are computed PURELY from <see cref="OhgShowViewModel.Current"/> and re-raised at the end
/// of every <see cref="OhgShowViewModel.Apply"/> via the <see cref="OnAppliedGfx"/> hook.
/// </summary>
public sealed partial class OhgShowViewModel
{
    // ── headline editor fields ──────────────────────────────────────────────────────────

    [ObservableProperty] private string headlineName = "";
    [ObservableProperty] private string headlineLocation = "";

    // ── override editor fields ──────────────────────────────────────────────────────────

    [ObservableProperty] private string overridePin = "";
    [ObservableProperty] private string overrideName = "";
    [ObservableProperty] private string overrideLocation = "";
    [ObservableProperty] private string overrideRole = "panelist";

    // ── lamps + health label (raised in OnAppliedGfx) ──────────────────────────────────

    [ObservableProperty] private string registryLamp = "unavailable";
    [ObservableProperty] private string? registryLampDetail;
    [ObservableProperty] private string handsLamp = "unavailable";
    [ObservableProperty] private string? handsLampDetail;
    [ObservableProperty] private string questionLamp = "unavailable";
    [ObservableProperty] private string? questionLampDetail;
    [ObservableProperty] private string mukanaHealthLabel = "failing";

    // ── overlay-derived read-only state ────────────────────────────────────────────────

    public string? QuestionText => Current?.Overlays.QuestionText;
    public string? QuestionAsker => Current?.Overlays.QuestionAsker;
    public bool HeadlineVisible => Current?.Overlays.HeadlineVisible ?? false;

    // ── apply hook (Task 3's Apply calls this after every applied snapshot) ────────────

    partial void OnAppliedGfx(OhgSnapshotView view)
    {
        RegistryLamp = view.Registry.State;
        RegistryLampDetail = view.Registry.Detail;
        HandsLamp = view.HandsQueue.State;
        HandsLampDetail = view.HandsQueue.Detail;
        QuestionLamp = view.QuestionFeed.State;
        QuestionLampDetail = view.QuestionFeed.Detail;
        MukanaHealthLabel = view.Health.Worst;

        OnPropertyChanged(nameof(QuestionText));
        OnPropertyChanged(nameof(QuestionAsker));
        OnPropertyChanged(nameof(HeadlineVisible));

        // Gallery.cs's computed SmartGallery — raised here because this is Apply's last hook and
        // the gallery partial has none of its own.
        OnPropertyChanged(nameof(SmartGallery));

        // Seed the headline editor from the snapshot's own headline ONLY when the operator has not
        // started typing anything — never clobber a mid-edit. Once seeded (or once the operator
        // types anything), later snapshots leave the fields alone.
        if (string.IsNullOrEmpty(HeadlineName) && string.IsNullOrEmpty(HeadlineLocation))
        {
            HeadlineName = view.Overlays.HeadlineName ?? "";
            HeadlineLocation = view.Overlays.HeadlineLocation ?? "";
        }
    }

    // ── headline commands ───────────────────────────────────────────────────────────────

    [RelayCommand]
    private async Task HeadlineInAsync()
    {
        var (id, args) = OhgActionArgs.HeadlineIn();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task HeadlineOutAsync()
    {
        var (id, args) = OhgActionArgs.HeadlineOut();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    /// <summary>Uses the two editor fields. Refused LOCALLY (no invoke) unless both are non-blank —
    /// the engine would refuse an incomplete headline anyway.</summary>
    [RelayCommand]
    private async Task HeadlineChangeAsync()
    {
        if (string.IsNullOrWhiteSpace(HeadlineName) || string.IsNullOrWhiteSpace(HeadlineLocation))
        {
            LastActionStatus = "Headline needs a name and a location";
            return;
        }

        var (id, args) = OhgActionArgs.HeadlineChange(HeadlineName, HeadlineLocation);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    // ── question commands ───────────────────────────────────────────────────────────────

    [RelayCommand]
    private async Task QuestionInAsync()
    {
        var (id, args) = OhgActionArgs.QuestionIn();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    [RelayCommand]
    private async Task QuestionOutAsync()
    {
        var (id, args) = OhgActionArgs.QuestionOut();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    // ── mukana commands ─────────────────────────────────────────────────────────────────

    [RelayCommand]
    private async Task MukanaSyncAsync()
    {
        var (id, args) = OhgActionArgs.MukanaSync();
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    /// <summary>Refused LOCALLY (no invoke) unless all four override fields are non-blank.</summary>
    [RelayCommand]
    private async Task OverrideSetAsync()
    {
        if (string.IsNullOrWhiteSpace(OverridePin) ||
            string.IsNullOrWhiteSpace(OverrideName) ||
            string.IsNullOrWhiteSpace(OverrideLocation) ||
            string.IsNullOrWhiteSpace(OverrideRole))
        {
            LastActionStatus = "Override needs PIN, name, location, and role";
            return;
        }

        var (id, args) = OhgActionArgs.OverrideSet(OverridePin, OverrideName, OverrideLocation, OverrideRole);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }

    /// <summary>Refused LOCALLY (no invoke) unless the PIN field is non-blank.</summary>
    [RelayCommand]
    private async Task OverrideDeleteAsync()
    {
        if (string.IsNullOrWhiteSpace(OverridePin))
        {
            LastActionStatus = "Override delete needs a PIN";
            return;
        }

        var (id, args) = OhgActionArgs.OverrideDelete(OverridePin);
        var result = await _invoker.InvokeAsync(id, args).ConfigureAwait(false);
        ReportResult(result);
    }
}
