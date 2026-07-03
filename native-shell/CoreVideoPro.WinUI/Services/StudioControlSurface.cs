using CoreVideoPro.Control;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Dispatching;
using System.ComponentModel;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Adapts the transport-agnostic <see cref="IControlSurface"/> onto the operator
/// <see cref="StudioViewModel"/>. Every remote invocation is marshaled onto the UI thread (the
/// captured <see cref="DispatcherQueue"/>) before touching a command or bound property — otherwise
/// it would trip the documented CoreMessagingXP 0xc000027b fail-fast. Feedback is pushed (debounced)
/// on relevant <see cref="INotifyPropertyChanged"/> changes. This is the only place the control
/// layer meets WinUI; the OSC/HTTP transports never see the ViewModel.</summary>
public sealed class StudioControlSurface : IControlSurface, IDisposable
{
    private static readonly TimeSpan FeedbackDebounce = TimeSpan.FromMilliseconds(150);

    // VM properties whose change should push fresh feedback. Deliberately curated so high-rate
    // properties (surfaces, fps, meters) do not spam the transport.
    private static readonly HashSet<string> FeedbackProps = new(StringComparer.Ordinal)
    {
        nameof(StudioViewModel.Recording), nameof(StudioViewModel.Streaming),
        nameof(StudioViewModel.ZoomCaptureSubscribed), nameof(StudioViewModel.EngineStatus),
        nameof(StudioViewModel.ZoomStatus), nameof(StudioViewModel.CommandStatus),
        nameof(StudioViewModel.ActiveSceneId), nameof(StudioViewModel.PreviewSceneId),
        nameof(StudioViewModel.CanTake), nameof(StudioViewModel.ViewMode),
        nameof(StudioViewModel.MultiviewLayoutMode), nameof(StudioViewModel.MultiviewTileCount),
        nameof(StudioViewModel.MultiviewShowLabels), nameof(StudioViewModel.MultiviewShowTally),
        nameof(StudioViewModel.MultiviewShowMeters), nameof(StudioViewModel.MultiviewShowClock),
        nameof(StudioViewModel.AutomationAutoTakeEnabled), nameof(StudioViewModel.AutomationAutoAssignInputsEnabled),
        nameof(StudioViewModel.AutomationLowerThirdsEnabled), nameof(StudioViewModel.AutomationCaptionsEnabled),
        nameof(StudioViewModel.AudioMonitoringEnabled), nameof(StudioViewModel.AudioMonitorVolume),
        nameof(StudioViewModel.MasterLimiterEnabled), nameof(StudioViewModel.ProgramLowerThirdKey),
        nameof(StudioViewModel.ProgramLowerThirdEnabled), nameof(StudioViewModel.ShowInputSummary),
        nameof(StudioViewModel.ProductionMode),
    };

    /// <summary>Every action id this adapter maps onto the ViewModel. A test asserts this covers
    /// <see cref="ControlActionRegistry"/> so a new registry action can't silently be unhandled.</summary>
    public static IReadOnlySet<string> SupportedActionIds { get; } = new HashSet<string>(StringComparer.Ordinal)
    {
        "transport.take", "transport.transition.set",
        "transport.record.toggle", "transport.record.set",
        "transport.stream.toggle", "transport.stream.set",
        "transport.engine.toggle", "transport.engine.set",
        "scene.select", "view.setMode",
        "input.assign", "input.name", "input.inShow.set",
        "graphics.lowerThird.toggle", "graphics.lowerThird.set", "graphics.caption.set", "graphics.graphic.toggle",
        "audio.monitor.set", "audio.monitor.volume", "audio.masterLimiter.set",
        "multiview.layout.set", "multiview.tileCount.set",
        "multiview.showLabels.set", "multiview.showTally.set", "multiview.showMeters.set", "multiview.showClock.set",
        "automation.toggle", "automation.autoTake.set", "automation.autoAssignInputs.set",
        "automation.lowerThirds.set", "automation.captions.set",
    };

    private readonly StudioViewModel _vm;
    private readonly DispatcherQueue _dispatcher;
    private readonly DispatcherQueueTimer _feedbackTimer;
    private bool _disposed;

    public StudioControlSurface(StudioViewModel viewModel, DispatcherQueue dispatcher)
    {
        _vm = viewModel;
        _dispatcher = dispatcher;
        _vm.PropertyChanged += OnViewModelPropertyChanged;
        _feedbackTimer = _dispatcher.CreateTimer();
        _feedbackTimer.IsRepeating = false;
        _feedbackTimer.Interval = FeedbackDebounce;
        _feedbackTimer.Tick += (_, _) => StateChanged?.Invoke(this, GetState());
    }

    public event EventHandler<ControlState>? StateChanged;

    public Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> args, CancellationToken cancellationToken = default)
    {
        var tcs = new TaskCompletionSource<ControlInvokeResult>(TaskCreationOptions.RunContinuationsAsynchronously);

        async void RunOnUi()
        {
            try
            {
                tcs.TrySetResult(await DispatchAsync(actionId, args).ConfigureAwait(true));
            }
            catch (Exception ex)
            {
                tcs.TrySetResult(ControlInvokeResult.Fail(ex.Message));
            }
        }

        if (_dispatcher.HasThreadAccess)
        {
            RunOnUi();
        }
        else if (!_dispatcher.TryEnqueue(RunOnUi))
        {
            tcs.TrySetResult(ControlInvokeResult.Fail("Could not marshal onto the UI thread."));
        }

        return tcs.Task;
    }

    // Runs ON the UI thread. Maps a stable action id onto the concrete VM command/property.
    private async Task<ControlInvokeResult> DispatchAsync(string actionId, IReadOnlyList<object?> args)
    {
        switch (actionId)
        {
            // ---- Transport ----------------------------------------------------------
            case "transport.take":
                if (_vm.TakeCommand.CanExecute(null))
                {
                    await _vm.TakeCommand.ExecuteAsync(null).ConfigureAwait(true);
                }
                return ControlInvokeResult.Success;
            case "transport.transition.set":
                _vm.SetTakeTransitionCommand.Execute(Str(args, 0));
                return ControlInvokeResult.Success;
            case "transport.record.toggle":
                await ExecuteIfCan(_vm.ToggleRecordingCommand).ConfigureAwait(true);
                return ControlInvokeResult.Success;
            case "transport.record.set":
                if (_vm.Recording != Bool(args, 0))
                {
                    await ExecuteIfCan(_vm.ToggleRecordingCommand).ConfigureAwait(true);
                }
                return ControlInvokeResult.Success;
            case "transport.stream.toggle":
                await ExecuteIfCan(_vm.ToggleStreamingCommand).ConfigureAwait(true);
                return ControlInvokeResult.Success;
            case "transport.stream.set":
                if (_vm.Streaming != Bool(args, 0))
                {
                    await ExecuteIfCan(_vm.ToggleStreamingCommand).ConfigureAwait(true);
                }
                return ControlInvokeResult.Success;
            case "transport.engine.toggle":
                await ExecuteIfCan(_vm.ToggleEngineCommand).ConfigureAwait(true);
                return ControlInvokeResult.Success;
            case "transport.engine.set":
                if (_vm.ZoomCaptureSubscribed != Bool(args, 0))
                {
                    await ExecuteIfCan(_vm.ToggleEngineCommand).ConfigureAwait(true);
                }
                return ControlInvokeResult.Success;

            // ---- Scenes / view ------------------------------------------------------
            case "scene.select":
                _vm.SelectSceneCommand.Execute(Str(args, 0));
                return ControlInvokeResult.Success;
            case "view.setMode":
                _vm.SetViewModeCommand.Execute(Str(args, 0));
                return ControlInvokeResult.Success;

            // ---- Show inputs --------------------------------------------------------
            case "input.assign":
                return AssignInput(Int(args, 0), Str(args, 1));
            case "input.name":
                return NameInput(Int(args, 0), args.Count > 1 ? args[1] as string : null);
            case "input.inShow.set":
                return SetInputInShow(Int(args, 0), Bool(args, 1));

            // ---- Graphics -----------------------------------------------------------
            case "graphics.lowerThird.toggle":
                _vm.ToggleProgramLowerThirdCommand.Execute(null);
                return ControlInvokeResult.Success;
            case "graphics.lowerThird.set":
                if (_vm.ProgramLowerThirdEnabled != Bool(args, 0))
                {
                    _vm.ToggleProgramLowerThirdCommand.Execute(null);
                }
                return ControlInvokeResult.Success;
            case "graphics.caption.set":
                _vm.CaptionText = Str(args, 0);
                if (args.Count > 1 && args[1] is string speaker)
                {
                    _vm.CaptionSpeaker = speaker;
                }
                return ControlInvokeResult.Success;
            case "graphics.graphic.toggle":
                await _vm.ToggleGraphicCommand.ExecuteAsync(Str(args, 0)).ConfigureAwait(true);
                return ControlInvokeResult.Success;

            // ---- Audio --------------------------------------------------------------
            case "audio.monitor.set":
                _vm.AudioMonitoringEnabled = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "audio.monitor.volume":
                _vm.AudioMonitorVolume = Math.Clamp(Double(args, 0), 0.0, 1.0);
                return ControlInvokeResult.Success;
            case "audio.masterLimiter.set":
                _vm.MasterLimiterEnabled = Bool(args, 0);
                return ControlInvokeResult.Success;

            // ---- Multiview ----------------------------------------------------------
            case "multiview.layout.set":
                _vm.MultiviewLayoutMode = StudioViewModel.NormalizeMultiviewLayoutMode(Str(args, 0));
                return ControlInvokeResult.Success;
            case "multiview.tileCount.set":
                _vm.MultiviewTileCount = StudioViewModel.ClampMultiviewTileCount(Int(args, 0));
                return ControlInvokeResult.Success;
            case "multiview.showLabels.set":
                _vm.MultiviewShowLabels = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "multiview.showTally.set":
                _vm.MultiviewShowTally = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "multiview.showMeters.set":
                _vm.MultiviewShowMeters = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "multiview.showClock.set":
                _vm.MultiviewShowClock = Bool(args, 0);
                return ControlInvokeResult.Success;

            // ---- Automation ---------------------------------------------------------
            case "automation.toggle":
                _vm.ToggleAutomationCommand.Execute(null);
                return ControlInvokeResult.Success;
            case "automation.autoTake.set":
                _vm.AutomationAutoTakeEnabled = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "automation.autoAssignInputs.set":
                _vm.AutomationAutoAssignInputsEnabled = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "automation.lowerThirds.set":
                _vm.AutomationLowerThirdsEnabled = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "automation.captions.set":
                _vm.AutomationCaptionsEnabled = Bool(args, 0);
                return ControlInvokeResult.Success;

            default:
                return ControlInvokeResult.Fail($"Action '{actionId}' is not implemented by the WinUI surface.");
        }
    }

    private ControlInvokeResult AssignInput(int slot, string sourceId)
    {
        if (!TryGetEditor(slot, out var editor))
        {
            return ControlInvokeResult.Fail($"Show Input slot {slot} is out of range (1-{_vm.ShowInputEditors.Count}).");
        }

        if (sourceId.StartsWith("zoom:", StringComparison.Ordinal))
        {
            editor.Kind = ShowInputKind.ZoomParticipant;
            editor.ParticipantId = sourceId["zoom:".Length..];
        }
        else if (sourceId.StartsWith("media:", StringComparison.Ordinal))
        {
            editor.Kind = ShowInputKind.Media;
            editor.ParticipantId = sourceId;  // Media stores the full "media:<id>" in ParticipantId.
        }
        else if (sourceId.StartsWith("capture:", StringComparison.Ordinal))
        {
            var deviceId = sourceId["capture:".Length..];
            editor.Kind = ResolveCaptureKind(deviceId);
            editor.CaptureDeviceId = deviceId;
        }
        else
        {
            return ControlInvokeResult.Fail($"Unrecognized source id '{sourceId}' (expected zoom:/capture:/media:).");
        }

        editor.InShow = true;
        return ControlInvokeResult.Success;
    }

    private ShowInputKind ResolveCaptureKind(string deviceId)
    {
        var device = _vm.CaptureDevices.FirstOrDefault(d => string.Equals(d.Id, deviceId, StringComparison.Ordinal));
        return device?.Vendor?.ToLowerInvariant() switch
        {
            "blackmagic" => ShowInputKind.Blackmagic,
            "aja" => ShowInputKind.Aja,
            "srt" => ShowInputKind.SrtIngest,
            _ => ShowInputKind.UvcWebcam
        };
    }

    private ControlInvokeResult NameInput(int slot, string? name)
    {
        if (!TryGetEditor(slot, out var editor))
        {
            return ControlInvokeResult.Fail($"Show Input slot {slot} is out of range.");
        }

        editor.DisplayName = name ?? string.Empty;  // blank resets to the derived name
        return ControlInvokeResult.Success;
    }

    private ControlInvokeResult SetInputInShow(int slot, bool inShow)
    {
        if (!TryGetEditor(slot, out var editor))
        {
            return ControlInvokeResult.Fail($"Show Input slot {slot} is out of range.");
        }

        editor.InShow = inShow;
        return ControlInvokeResult.Success;
    }

    private bool TryGetEditor(int slot, out ShowInputSlotViewModel editor)
    {
        // Slots are 1-based on the wire.
        var index = slot - 1;
        if (index >= 0 && index < _vm.ShowInputEditors.Count)
        {
            editor = _vm.ShowInputEditors[index];
            return true;
        }

        editor = null!;
        return false;
    }

    public ControlState GetState()
    {
        var inputs = _vm.ShowInputEditors
            .Select(editor => new ControlInputState(
                editor.SlotNumber,
                editor.InShow,
                editor.Kind.ToString(),
                editor.SourceId,
                editor.DisplayName))
            .ToList();

        return new ControlState
        {
            Recording = _vm.Recording,
            Streaming = _vm.Streaming,
            EngineOn = _vm.ZoomCaptureSubscribed,
            CanTake = _vm.CanTake,
            LowerThirdOnAir = _vm.ProgramLowerThirdKey.IsVisible,
            ZoomStatus = _vm.ZoomStatus,
            EngineStatus = _vm.EngineStatus,
            CommandStatus = _vm.CommandStatus,
            ActiveSceneId = _vm.ActiveSceneId,
            PreviewSceneId = _vm.PreviewSceneId,
            ViewMode = _vm.ViewMode.ToString(),
            MultiviewLayoutMode = _vm.MultiviewLayoutMode,
            MultiviewTileCount = _vm.MultiviewTileCount,
            AutomationOn = _vm.ProductionMode != ProductionMode.Manual,
            AutoTake = _vm.AutomationAutoTakeEnabled,
            AutoAssignInputs = _vm.AutomationAutoAssignInputsEnabled,
            AutoLowerThirds = _vm.AutomationLowerThirdsEnabled,
            AutoCaptions = _vm.AutomationCaptionsEnabled,
            AudioMonitorOn = _vm.AudioMonitoringEnabled,
            AudioMonitorVolume = _vm.AudioMonitorVolume,
            MasterLimiterOn = _vm.MasterLimiterEnabled,
            Inputs = inputs,
        };
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_disposed || e.PropertyName is null || !FeedbackProps.Contains(e.PropertyName))
        {
            return;
        }

        // Debounce on the UI thread — coalesce a burst of property changes into one push.
        _feedbackTimer.Stop();
        _feedbackTimer.Start();
    }

    private static async Task ExecuteIfCan(CommunityToolkit.Mvvm.Input.IAsyncRelayCommand command)
    {
        if (command.CanExecute(null))
        {
            await command.ExecuteAsync(null).ConfigureAwait(true);
        }
    }

    private static string Str(IReadOnlyList<object?> args, int index) => args.Count > index && args[index] is string s ? s : string.Empty;

    private static bool Bool(IReadOnlyList<object?> args, int index) => args.Count > index && args[index] is bool b && b;

    private static int Int(IReadOnlyList<object?> args, int index) => args.Count > index && args[index] is int i ? i : 0;

    private static double Double(IReadOnlyList<object?> args, int index) => args.Count > index && args[index] is double d ? d : 0.0;

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _vm.PropertyChanged -= OnViewModelPropertyChanged;
        _feedbackTimer.Stop();
    }
}
