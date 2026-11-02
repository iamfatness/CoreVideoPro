using CoreVideoPro.Control;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.ViewModels.Transport;
using Microsoft.UI.Dispatching;
using System.ComponentModel;
using System.Text.Json;
using GraphicsPlaylistEntry = CoreVideoPro.MediaCore.Models.GraphicsPlaylistEntry;
using GraphicsPlaylistRow = CoreVideoPro.MediaCore.Models.GraphicsPlaylistRow;
using NativeGraphicsOperationResult = CoreVideoPro.MediaCore.Models.NativeGraphicsOperationResult;
using ShotLayout = CoreVideoPro.MediaCore.Models.ShotLayout;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Adapts the transport-agnostic <see cref="IControlSurface"/> onto the operator
/// <see cref="StudioViewModel"/>. Every remote invocation is marshaled onto the UI thread (the
/// captured <see cref="DispatcherQueue"/>) before touching a command or bound property — otherwise
/// it would trip the documented CoreMessagingXP 0xc000027b fail-fast. Feedback is pushed (debounced)
/// on relevant <see cref="INotifyPropertyChanged"/> changes. This is the only place the control
/// layer meets WinUI; the OSC/HTTP transports never see the ViewModel.</summary>
public sealed class StudioControlSurface : IControlSurface, INativeSnapshotObserver, IDisposable
{
    private static readonly TimeSpan FeedbackDebounce = TimeSpan.FromMilliseconds(150);

    // VM properties whose change should push fresh feedback. Deliberately curated so high-rate
    // properties (surfaces, fps, meters) do not spam the transport.
    private static readonly HashSet<string> FeedbackProps = new(StringComparer.Ordinal)
    {
        nameof(StudioViewModel.Recording), nameof(StudioViewModel.Streaming),
        nameof(StudioViewModel.NativeLowerThirdStatus),
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
        nameof(StudioViewModel.MasterLimiterEnabled), nameof(StudioViewModel.MasteringEnabled),
        nameof(StudioViewModel.MasteringTargetIndex), nameof(StudioViewModel.VirtualCameraEnabled),
        nameof(StudioViewModel.NdiOutputEnabled), nameof(StudioViewModel.NdiOutputStatusLabel),
        nameof(StudioViewModel.StreamNdiGraphicsKeyEnabled), nameof(StudioViewModel.StreamNdiGraphicsName),
        nameof(StudioViewModel.VstPluginHostSummary),
        nameof(StudioViewModel.ProgramLowerThirdKey),
        nameof(StudioViewModel.ProgramLowerThirdEnabled), nameof(StudioViewModel.ShowInputSummary),
        nameof(StudioViewModel.ProductionMode),
        nameof(StudioViewModel.ProgramBufferFrames), nameof(StudioViewModel.ProgramBufferRestartRequired),
        // Graphics playout (#1076): strip truth, guest entries and playlist edits light buttons.
        nameof(StudioViewModel.GraphicsStrip), nameof(StudioViewModel.GuestEntries), nameof(StudioViewModel.GraphicsPlayoutStatus),
    };

    /// <summary>transport.ndi.set / toggle: <see cref="StudioViewModel.SetNdiOutput"/> returns the
    /// refusal reason (NDI left as it was) or null when applied.</summary>
    public static ControlInvokeResult NdiSetResult(string? refusal) =>
        refusal is null ? ControlInvokeResult.Success : ControlInvokeResult.Fail(refusal);

    /// <summary>Every action id this adapter maps onto the ViewModel. A test asserts this covers
    /// <see cref="ControlActionRegistry"/> so a new registry action can't silently be unhandled.</summary>
    public static IReadOnlySet<string> SupportedActionIds { get; } = new HashSet<string>(StringComparer.Ordinal)
    {
        "zoom.join", "zoom.leave",
        "transport.take", "transport.transition.set",
        "transport.record.toggle", "transport.record.set",
        "transport.stream.toggle", "transport.stream.set",
        "transport.stream.rtmp.configure", "transport.stream.srt.configure",
        "transport.ndi.set", "transport.ndi.toggle", "transport.ndi.graphicsKey.set",
        "transport.engine.toggle", "transport.engine.set",
        "transport.virtualcam.toggle", "transport.virtualcam.set", "transport.virtualcam.mirror.set",
        "transport.virtualcam.name.set",
        "scene.select", "scene.cueWithRoutes", "scene.route.set", "scene.list", "scene.restoreStarters", "scene.dynamicGallery.create", "view.setMode",
        "source.capture.online.set", "input.assign", "input.unassign", "input.name", "input.inShow.set", "source.dropout.set",
        "graphics.lowerThird.toggle", "graphics.lowerThird.set", "graphics.caption.set", "graphics.graphic.toggle",
        "graphics.instance.create", "graphics.instance.update", "graphics.instance.cue",
        "graphics.clock.control", "graphics.instance.trigger", "graphics.instance.remove", "graphics.program.clear",
        "graphics.template.select", "graphics.cue.create", "graphics.studio.assign", "graphics.image.import", "graphics.logo.import", "graphics.cue.select", "graphics.cue.action", "graphics.cue.save", "graphics.saved.restore",
        "graphics.template.install", "graphics.template.list", "graphics.authority.set", "graphics.automation.configure", "graphics.automation.control",
        // Graphics playout (#1076): the ids the Graphics tab, the F-key router and Companion share.
        "graphics.playlist.list", "graphics.playlist.clock", "graphics.playlist.row.set", "graphics.playlist.row.remove", "graphics.playlist.hotkey.set", "graphics.playlist.default",
        "graphics.playIn", "graphics.toPreview", "graphics.playOut", "graphics.preview.remove", "graphics.playOutAll", "graphics.preview.clear", "graphics.strip",
        "graphics.layout.list", "graphics.layout.set", "graphics.layout.remove", "graphics.layout.cue", "graphics.layout.take",
        "audio.zoomMode.set", "audio.monitor.set", "audio.monitor.volume", "audio.route.set", "audio.masterLimiter.set",
        "audio.mastering.set", "audio.mastering.target", "audio.vst.scan",
        "multiview.layout.set", "multiview.tileCount.set",
        "multiview.showLabels.set", "multiview.showTally.set", "multiview.showMeters.set", "multiview.showClock.set",
        "automation.magic", "automation.toggle", "automation.autoTake.set", "automation.autoAssignInputs.set",
        "automation.lowerThirds.set", "automation.captions.set",
        "browser.add", "browser.remove", "browser.reload",
        "settings.programBuffer.set",
        "scene.tiles.autofill.set", "scene.tiles.slot.assign", "scene.tiles.exclusion.set",
        "scene.tiles.background.set", "scene.tiles.override.set", "scene.tiles.override.clear",
        "scene.tiles.names.set",
        // Shell-owned OHG action (spec §6.3): restarting the show engine is a SHELL concern, not
        // an engine action — a dead engine could not answer one. Every `ohg.*` id comes from the
        // engine's own manifest at handshake and is deliberately NOT listed here; the 1:1
        // coverage test pins this set against the STATIC registry only.
        "showEngine.restart",
    };

    private readonly StudioViewModel _vm;
    private readonly DispatcherQueue _dispatcher;
    private readonly DispatcherQueueTimer _feedbackTimer;
    private readonly ShowEngineBridge? _bridge;
    /// <summary>The OHG host adapter, held in a SLOT rather than a readonly field so a saved show
    /// config can swap it live (<see cref="ReplaceOhgAdapter"/>). Read at APPLY time — see
    /// <see cref="OhgAdapterSlot"/>.</summary>
    private readonly OhgAdapterSlot _ohgAdapterSlot = new();
    private bool _disposed;

    /// <summary>The OHG show engine is OPTIONAL (both trailing arguments null = the app has no
    /// show config, which is the default). Nothing about the existing surface changes when it is
    /// absent; an `ohg.*` invoke simply fails with a plain reason.</summary>
    public StudioControlSurface(
        StudioViewModel viewModel,
        DispatcherQueue dispatcher,
        ShowEngineBridge? bridge = null,
        OhgHostAdapter? ohgAdapter = null)
    {
        _vm = viewModel;
        _dispatcher = dispatcher;
        _bridge = bridge;
        _ohgAdapterSlot.Replace(ohgAdapter);
        _feedbackTimer = _dispatcher.CreateTimer();
        _feedbackTimer.IsRepeating = false;
        _feedbackTimer.Interval = FeedbackDebounce;
        _feedbackTimer.Tick += (_, _) => StateChanged?.Invoke(this, GetState());
        // Create the playout service here, on the UI thread, so GetState (which transports may
        // call from their own threads) only ever reads an existing strip and never lazily builds it.
        // Build the playout service AFTER the feedback timer exists and BEFORE we
        // subscribe: its first Observe raises property changes that the handler
        // debounces through _feedbackTimer (2026-10-31 startup NullReference fail-fast).
        _ = _vm.GraphicsPlayout;
        _vm.PropertyChanged += OnViewModelPropertyChanged;

        if (_bridge is null)
        {
            return;
        }

        // ALL THREE fire on the show engine's READER THREAD (spec §6.1). Every one of them is
        // marshaled through UiDispatch, never a raw TryEnqueue with a throwing body — a throwing
        // queued callback fail-fasts the process with no managed log (CLAUDE.md).
        _bridge.SnapshotChanged += OnBridgeSnapshotChanged;
        _bridge.HealthChanged += OnBridgeHealthChanged;
        _bridge.HostCommand += OnBridgeHostCommand;
    }

    public event EventHandler<ControlState>? StateChanged;

    /// <summary>True for an action the OHG show engine owns. Spec §3: `ohg.*` invokes never
    /// touch the UI thread — they go straight to the bridge, which talks to another process.
    /// The DOT is part of the prefix on purpose: "ohgx.y" is somebody else's namespace and must
    /// keep falling through to the ViewModel switch (where it fails honestly).</summary>
    /// <summary><c>graphics.playOut</c> takes the instance id either as the registry's third
    /// parameter or, the way the playlist/strip chips and OSC send it, as the only argument.
    /// Returns "" when the call names a playlist row instead (rowId [+ sourceId]).</summary>
    public static string PlayOutInstanceId(IReadOnlyList<object?> args)
    {
        var third = Str(args, 2);
        if (third.Length > 0) return third;
        var first = Str(args, 0);
        return first.StartsWith(GraphicsPlayoutService.InstancePrefix, StringComparison.Ordinal) ? first : string.Empty;
    }

    public static bool IsBridgeAction(string? id) => id is not null && id.StartsWith("ohg.", StringComparison.Ordinal);

    public Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> args, CancellationToken cancellationToken = default)
    {
        // Marshal-free paths, taken BEFORE the dispatcher enqueue below.
        if (IsBridgeAction(actionId))
        {
            return _bridge is null
                ? Task.FromResult(ControlInvokeResult.Fail(NoShowEngine))
                : _bridge.InvokeAsync(actionId, args, cancellationToken);
        }

        if (string.Equals(actionId, RestartActionId, StringComparison.Ordinal))
        {
            return RestartShowEngineAsync(cancellationToken);
        }

        var tcs = new TaskCompletionSource<ControlInvokeResult>(TaskCreationOptions.RunContinuationsAsynchronously);

        async void RunOnUi()
        {
            try
            {
                // A command may have queued before Dispose closed the surface.
                // Check on the UI thread, where disposal sets the gate, before
                // touching any VM command or property during teardown.
                if (_disposed)
                {
                    tcs.TrySetResult(ControlInvokeResult.Fail("Control surface is shutting down."));
                    return;
                }
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
            case "settings.programBuffer.set":
                return ApplyProgramBufferRequest(args.Count > 0 ? args[0] : null, _vm.SaveProgramBufferFrames);
            // ---- Zoom session -------------------------------------------------------
            case "zoom.join":
                _vm.Settings.JoinMeetingUrl = Str(args, 0);
                if (args.Count > 1 && args[1] is string displayName && !string.IsNullOrWhiteSpace(displayName))
                {
                    _vm.Settings.DisplayName = displayName;
                }
                _vm.Settings.IsWebinar = args.Count > 2 && args[2] is bool webinar && webinar;
                if (!_vm.Settings.JoinZoomCommand.CanExecute(null))
                {
                    return ControlInvokeResult.Fail("Zoom join is unavailable in the current state.");
                }
                await _vm.Settings.JoinZoomCommand.ExecuteAsync(null).ConfigureAwait(true);
                return _vm.Settings.IsInMeeting
                    ? ControlInvokeResult.Success
                    : ControlInvokeResult.Fail(_vm.Settings.JoinStatus);
            case "zoom.leave":
                if (!_vm.Settings.LeaveZoomCommand.CanExecute(null))
                {
                    return ControlInvokeResult.Fail("Zoom leave is unavailable in the current state.");
                }
                await _vm.Settings.LeaveZoomCommand.ExecuteAsync(null).ConfigureAwait(true);
                return !_vm.Settings.IsInMeeting
                    ? ControlInvokeResult.Success
                    : ControlInvokeResult.Fail(_vm.Settings.JoinStatus);

            // ---- Transport ----------------------------------------------------------
            case "transport.take":
                return await RunTake(_vm.TakeCommand.CanExecute(null), _vm.TakeForControlAsync).ConfigureAwait(true);
            case "transport.transition.set":
                _vm.SetTakeTransitionCommand.Execute(Str(args, 0));
                return ControlInvokeResult.Success;
            case "transport.record.toggle":
                return await RunTransportToggle(_vm.ToggleRecordingCommand, "recording").ConfigureAwait(true);
            case "transport.record.set":
                return await RunOutputSet(_vm.RecordingRequested, _vm.Recording, Bool(args, 0), _vm.CanSetRecording, _vm.SetRecordingAsync, "recording").ConfigureAwait(true);
            case "transport.stream.toggle":
                return await RunTransportToggle(_vm.ToggleStreamingCommand, "streaming").ConfigureAwait(true);
            case "transport.stream.rtmp.configure":
                return _vm.ConfigureStreamDestination(false, args);
            case "transport.stream.srt.configure":
                return _vm.ConfigureStreamDestination(true, args);
            // ---- NDI output (independent of streaming) -------------------------------
            case "transport.ndi.set":
                return NdiSetResult(_vm.SetNdiOutput(Bool(args, 0)));
            case "transport.ndi.toggle":
                return NdiSetResult(_vm.SetNdiOutput(!_vm.NdiOutputEnabled));
            case "transport.ndi.graphicsKey.set":
                // Validated first (NDI on, core capability, name), then applied as one edit with one
                // sync; the core adds or drops only the key sender.
                return _vm.SetNdiGraphicsKey(Bool(args, 0), args.Count > 1 ? args[1] as string : null);
            case "transport.stream.set":
                return await RunOutputSet(_vm.StreamingRequested, _vm.Streaming, Bool(args, 0), _vm.CanSetStreaming, _vm.SetStreamingAsync, "streaming").ConfigureAwait(true);
            case "transport.engine.toggle":
                return await RunTransportToggle(_vm.ToggleEngineCommand, "capture engine").ConfigureAwait(true);
            case "transport.engine.set":
                return await RunTransportSet(_vm.ZoomCaptureSubscribed, Bool(args, 0), _vm.ToggleEngineCommand, "capture engine").ConfigureAwait(true);
            case "transport.virtualcam.toggle":
                _vm.VirtualCameraEnabled = !_vm.VirtualCameraEnabled;
                return ControlInvokeResult.Success;
            case "transport.virtualcam.set":
                _vm.VirtualCameraEnabled = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "transport.virtualcam.mirror.set":
                _vm.VirtualCameraMirror = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "transport.virtualcam.name.set":
                _vm.VirtualCameraDeviceName = Str(args, 0);
                return ControlInvokeResult.Success;

            // ---- Scenes / view ------------------------------------------------------
            case "scene.select":
            {
                // #1070 review: an unknown id used to answer ok while the shell only wrote a
                // status line. Refuse-closed like cueWithRoutes, and report what was queued.
                var selectSceneId = Str(args, 0);
                if (SceneSelectArgs.Refuse(selectSceneId, _vm.Scenes.Select(scene => scene.Id).ToList()) is { } selectError)
                {
                    return ControlInvokeResult.Fail(selectError);
                }
                _vm.SelectSceneCommand.Execute(selectSceneId);
                return string.Equals(_vm.PreviewSceneId, selectSceneId, StringComparison.Ordinal)
                    ? ControlInvokeResult.AppliedPreview(_vm.PreviewSceneId)
                    : ControlInvokeResult.Fail($"Scene '{selectSceneId}' was not queued on Preview.");
            }
            case "scene.cueWithRoutes":
                if (!SceneCueWithRoutesArgs.TryParseRoutes(Str(args, 1), out var parsedRoutes, out var parseError))
                {
                    return ControlInvokeResult.Fail(parseError ?? "routes is not valid JSON.");
                }
                return CuePreviewWithRoutes(Str(args, 0), parsedRoutes);
            case "scene.route.set":
                return CuePreviewWithRoutes(Str(args, 0),
                    new Dictionary<string, int?>(StringComparer.Ordinal) { [Str(args, 1)] = Int(args, 2) });
            case "scene.list":
                return ControlInvokeResult.WithData(ScenesJson());
            case "scene.restoreStarters":
                _vm.RestoreStarterScenesCommand.Execute(null);
                return ControlInvokeResult.Success;
            case "scene.dynamicGallery.create":
                _vm.NewDynamicGalleryCommand.Execute(null);
                return ControlInvokeResult.Success;
            case "scene.tiles.autofill.set":
                _vm.SetTilesAutoFill(Bool(args, 0));
                return ControlInvokeResult.Success;
            case "scene.tiles.slot.assign":
                return ApplyTilesSlotRequest(Double(args, 0), Str(args, 1), _vm.AssignTilesSlot);
            case "scene.tiles.exclusion.set":
                _vm.SetTilesExclusion(Str(args, 0), Bool(args, 1));
                return ControlInvokeResult.Success;
            case "scene.tiles.background.set":
                _vm.SetTilesBackground(Str(args, 0), Str(args, 1));
                return ControlInvokeResult.Success;
            case "scene.tiles.override.set":
                _vm.SetTilesOverride(Str(args, 0), Double(args, 1), Double(args, 2), Double(args, 3), Double(args, 4), Double(args, 5), Double(args, 6), Double(args, 7));
                return ControlInvokeResult.Success;
            case "scene.tiles.override.clear":
                _vm.ClearTilesOverride(Str(args, 0));
                return ControlInvokeResult.Success;
            case "scene.tiles.names.set":
                _vm.SetTilesNames(Bool(args, 0));
                return ControlInvokeResult.Success;
            case "view.setMode":
                if (!ViewModeContract.TryNormalize(Str(args, 0), out var viewMode))
                {
                    return ControlInvokeResult.Fail($"Unsupported view mode. Use {ViewModeContract.SupportedModes}.");
                }
                _vm.SetViewModeCommand.Execute(viewMode);
                return ControlInvokeResult.Success;

            // ---- Show inputs --------------------------------------------------------
            case "input.assign":
                return AssignInput(Int(args, 0), Str(args, 1));
            case "source.capture.online.set":
            {
                var sourceId = Str(args,0);
                if (!sourceId.StartsWith("capture:",StringComparison.Ordinal)) return ControlInvokeResult.Fail("Choose a capture source ID.");
                var id = sourceId["capture:".Length..];
                if (!_vm.CaptureDevices.Any(d => d.Id == id)) return ControlInvokeResult.Fail("The capture device is unavailable.");
                var online = Bool(args,1);
                if (online) await _vm.ConnectCaptureDeviceCommand.ExecuteAsync(id);
                else await _vm.TakeCaptureDeviceOfflineCommand.ExecuteAsync(id);
                var device = _vm.CaptureDevices.FirstOrDefault(d => d.Id == id);
                return device is not null && (device.ConnectionState == CaptureConnectionState.Connected) == online
                    ? ControlInvokeResult.Success : ControlInvokeResult.Fail(_vm.CommandStatus);
            }
            case "input.unassign":
            {
                if (!TryGetEditor(Int(args,0),out var editor)) return ControlInvokeResult.Fail("Show input slot is out of range.");
                using var write = ShowInputWriteScope.Enter("control-api.unassign");
                _vm.UnassignShowInputCommand.Execute(editor); return ControlInvokeResult.Success;
            }
            case "input.name":
                return NameInput(Int(args, 0), args.Count > 1 ? args[1] as string : null);
            case "input.inShow.set":
                return SetInputInShow(Int(args, 0), Bool(args, 1));
            case "source.dropout.set":
            {
                // R6 (final review, #535 slice 4a): a dropout policy is
                // Zoom-only this slice — refuse a non-"zoom:" id here rather
                // than letting it fall through to the ViewModel's silent
                // CommandStatus-only no-op, so a control-surface caller (OSC,
                // Companion, a script) gets an explicit failure it can check.
                var sourceId = Str(args, 0);
                if (ValidateZoomOnlyDropoutSourceId(sourceId) is { } refusal)
                {
                    return refusal;
                }
                _vm.SetSourceDropoutPolicy(sourceId, Str(args, 1));
                return ControlInvokeResult.Success;
            }

            // ---- Graphics -----------------------------------------------------------
            case "graphics.template.select":
            {
                var workspace = _vm.GraphicsWorkspace;
                if (workspace.Busy) return ControlInvokeResult.Fail("Graphics is busy; private selection was kept.");
                var template = workspace.State?.Templates.FirstOrDefault(t => t.TemplateId == Str(args,0) && t.TemplateVersion == Int(args,1));
                if (template is null) return ControlInvokeResult.Fail("The requested template version is unavailable.");
                workspace.Browse(template); return ControlInvokeResult.Success;
            }
            case "graphics.cue.create":
            {
                var workspace = _vm.GraphicsWorkspace;
                var template = workspace.State?.Templates.FirstOrDefault(t => t.TemplateId == Str(args,0) && t.TemplateVersion == Int(args,1));
                if (template is null) return ControlInvokeResult.Fail("The requested template version is unavailable.");
                var scope = Str(args,2); var sceneId = Str(args,3);
                if (scope is not ("global" or "scene") || (scope == "scene" && !_vm.Scenes.Any(s => s.Id == sceneId)))
                    return ControlInvokeResult.Fail("Choose a global scope or an existing scene.");
                return await workspace.CreateAsync(template,scope,scope == "global" ? "" : sceneId)
                    ? ControlInvokeResult.Success : ControlInvokeResult.Fail(workspace.Status);
            }
            case "graphics.studio.assign":
                return await _vm.AssignGraphicsLookToStudioAsync(Bool(args,0)) ? ControlInvokeResult.Success : ControlInvokeResult.Fail(_vm.CommandStatus);
            case "graphics.cue.select":
                if (_vm.GraphicsWorkspace.Busy || _vm.GraphicsWorkspace.State?.Instances.Any(i=>i.InstanceId==Str(args,0)) != true)
                    return ControlInvokeResult.Fail("The cue is unavailable or busy.");
                _vm.GraphicsWorkspace.Select(Str(args,0)); return ControlInvokeResult.Success;
            case "graphics.cue.action":
            {
                var workspace=_vm.GraphicsWorkspace;
                var accepted=Str(args,0) switch {
                    "cue" => await workspace.CueAsync(), "show" => await workspace.ShowAsync(),
                    "hide" => await workspace.HideAsync(), "update" => await workspace.UpdateProgramAsync(), _ => false
                };
                return accepted ? ControlInvokeResult.Success : ControlInvokeResult.Fail(workspace.Status);
            }
            case "graphics.cue.save":
                return await _vm.GraphicsLibrary.SaveCueAsync(Str(args,0)) ? ControlInvokeResult.Success : ControlInvokeResult.Fail(_vm.GraphicsLibrary.Status);
            case "graphics.saved.restore":
            {
                await _vm.GraphicsLibrary.LoadAsync();
                var saved=_vm.GraphicsLibrary.Document.Cues.FirstOrDefault(c=>c.Id==Str(args,0));
                return saved is not null && await _vm.GraphicsLibrary.RestoreAsync(saved)
                    ? ControlInvokeResult.Success : ControlInvokeResult.Fail(_vm.GraphicsLibrary.Status);
            }
            case "graphics.logo.import":
                return await _vm.GraphicsWorkspace.ImportLogoAsync(Str(args,0))
                    ? ControlInvokeResult.Success : ControlInvokeResult.Fail(_vm.GraphicsWorkspace.ImportStatus);
            case "graphics.image.import":
            {
                var workspace = _vm.GraphicsWorkspace;
                var selected = workspace.SelectedId; var draft = workspace.Draft;
                var image = await workspace.ImportImageAsync(Str(args,0),fullCanvas:true);
                var template = image is null ? null : workspace.State?.Templates.FirstOrDefault(t => t.TemplateId == image.TemplateId && t.TemplateVersion == image.TemplateVersion);
                if (template is null) return ControlInvokeResult.Fail(workspace.ImportStatus);
                if (workspace.SelectedId != selected || workspace.Draft != draft) return ControlInvokeResult.Fail("Graphic imported; a newer private selection or edit was preserved.");
                workspace.Browse(template); return ControlInvokeResult.Success;
            }
            case "graphics.instance.create":
            case "graphics.instance.update":
            case "graphics.instance.cue":
            case "graphics.instance.trigger":
            case "graphics.clock.control":
            case "graphics.instance.remove":
            case "graphics.program.clear":
            case "graphics.template.install":
            case "graphics.automation.configure":
            case "graphics.automation.control":
            case "graphics.authority.set":
            case "graphics.template.list":
            {
                var result = await _vm.InvokeGraphicsActionAsync(actionId, Str(args, 0)).ConfigureAwait(true);
                return result.Accepted ? ControlInvokeResult.Success : ControlInvokeResult.Fail(result.Reason);
            }
            case "graphics.lowerThird.toggle":
            case "graphics.lowerThird.set":
            {
                // With a playlist Guest row the ViewModel routes this into graphics.playIn/playOut
                // of the Program subject's entry (#1076 C1); otherwise the legacy/managed path.
                var visible = _vm.LowerThirdIntentVisible;
                var result = await _vm.ApplyStudioLowerThirdIntentAsync(actionId.EndsWith("toggle", StringComparison.Ordinal) ? !visible : Bool(args,0)).ConfigureAwait(true);
                return result.Accepted ? ControlInvokeResult.Success : ControlInvokeResult.Fail(result.Reason);
            }

            // ---- Graphics playout (#1076) ------------------------------------------
            case "graphics.playlist.list":
                await _vm.GraphicsLibrary.LoadAsync().ConfigureAwait(true);
                return ControlInvokeResult.WithData(PlaylistJson());
            case "graphics.playlist.clock":
            {
                if (_vm.GraphicsPlayout.Row(Str(args, 0)) is not { } clockRow) return ControlInvokeResult.Fail("That playlist row does not exist.");
                return Graphics(await _vm.GraphicsPlayout.ClockAsync(GraphicsPlayoutService.EntryFor(clockRow), Str(args, 1)).ConfigureAwait(true));
            }
            case "graphics.playlist.row.set":
            {
                GraphicsPlaylistRow? row;
                try { row = JsonSerializer.Deserialize<GraphicsPlaylistRow>(Str(args, 0), PlayoutJson); }
                catch (JsonException ex) { return ControlInvokeResult.Fail($"rowJson is not a playlist row: {ex.Message}"); }
                if (row is null || !row.IsValid) return ControlInvokeResult.Fail("rowJson is not a valid playlist row (id a-z0-9-, kind guest|title|logo|liveBug|sponsor, name, hotkey F1..F12 or blank, playOutAfterMs 0|8000|15000).");
                await _vm.GraphicsLibrary.LoadAsync().ConfigureAwait(true);
                var rows = _vm.GraphicsPlayout.Playlist.ToList();
                var index = rows.FindIndex(r => r.Id == row.Id);
                if (index < 0) rows.Add(row); else rows[index] = row;
                return await SavePlaylist(rows).ConfigureAwait(true);
            }
            case "graphics.playlist.row.remove":
            {
                await _vm.GraphicsLibrary.LoadAsync().ConfigureAwait(true);
                var rows = _vm.GraphicsPlayout.Playlist.ToList();
                if (rows.RemoveAll(r => r.Id == Str(args, 0)) == 0) return ControlInvokeResult.Fail("That playlist row does not exist.");
                return await SavePlaylist(rows).ConfigureAwait(true);
            }
            case "graphics.playlist.hotkey.set":
            {
                var rowId = Str(args, 0); var key = Str(args, 1).Trim().ToUpperInvariant();
                if (!GraphicsPlaylistRow.ValidHotkey(key)) return ControlInvokeResult.Fail("key must be F1..F12 or blank.");
                await _vm.GraphicsLibrary.LoadAsync().ConfigureAwait(true);
                var rows = _vm.GraphicsPlayout.Playlist;
                if (rows.All(r => r.Id != rowId)) return ControlInvokeResult.Fail("That playlist row does not exist.");
                // One key, one row: a key already assigned elsewhere moves here.
                return await SavePlaylist(rows.Select(r => r.Id == rowId ? r with { Hotkey = key } : key.Length > 0 && r.Hotkey == key ? r with { Hotkey = "" } : r).ToList()).ConfigureAwait(true);
            }
            case "graphics.playlist.default":
            {
                await _vm.GraphicsLibrary.LoadAsync().ConfigureAwait(true);
                if (_vm.GraphicsPlayout.Playlist.Count > 0) return ControlInvokeResult.Fail("A graphics playlist already exists; edit its rows instead.");
                var starterId = Str(args, 0);
                var starter = starterId.Length == 0 ? GraphicsShowStarters.All[0] : GraphicsShowStarters.All.FirstOrDefault(s => s.Id == starterId);
                if (starter is null) return ControlInvokeResult.Fail($"Unknown show starter '{starterId}'. Use one of: {string.Join(", ", GraphicsShowStarters.All.Select(s => s.Id))}.");
                if (_vm.GraphicsWorkspace.State is not { CoreEpoch.Length: > 0 } graphicsState)
                {
                    // Say which link is missing: the core publishes graphics, so a refusal here is a shell-side gap.
                    var last = _vm.LastCoreSnapshotForDiagnostics;
                    var detail = last is null ? "no core snapshot observed yet"
                        : last.Graphics is null ? "the typed snapshot carries no graphics section (raw has it: " + (last.RawJson?.Contains("\"coreEpoch\"", StringComparison.Ordinal) == true) + ")"
                        : last.Graphics.CoreEpoch.Length == 0 ? "graphics section has an empty coreEpoch"
                        : "graphics workspace session not yet observed (State " + (_vm.GraphicsWorkspace.State is null ? "null" : "present") + ")";
                    return ControlInvokeResult.Fail("Native Graphics is unavailable: " + detail + ".");
                }
                // The preset shot layouts (Open, Two-up, Panel, Clean) install with the default rows;
                // layouts the operator already saved are kept.
                var presetLayouts = _vm.GraphicsPlayout.Layouts.Count == 0 ? GraphicsWorkspaceLibrary.DefaultShotLayouts() : null;
                try
                {
                    var rows = GraphicsWorkspaceLibrary.DefaultPlaylistFor(starter, graphicsState);
                    return await _vm.GraphicsLibrary.SavePlaylistAsync(rows, presetLayouts).ConfigureAwait(true) ? ControlInvokeResult.Success : ControlInvokeResult.Fail(_vm.GraphicsLibrary.Status);
                }
                catch (InvalidOperationException ex) { return ControlInvokeResult.Fail(ex.Message); }
            }
            case "graphics.playIn":
            case "graphics.toPreview":
            {
                if (ResolvePlaylistEntry(Str(args, 0), Str(args, 1), out var entry) is { } refusal) return refusal;
                var result = actionId == "graphics.playIn" ? await _vm.GraphicsPlayout.PlayInAsync(entry!).ConfigureAwait(true) : await _vm.GraphicsPlayout.ToPreviewAsync(entry!).ConfigureAwait(true);
                return Graphics(result);
            }
            case "graphics.playOut":
            {
                // The playlist/strip chips (and OSC) pass the instance id as the only argument; the
                // registry's named form puts it third. Either spelling hides that one instance.
                var instanceId = PlayOutInstanceId(args);
                if (instanceId.Length > 0) return Graphics(await _vm.GraphicsPlayout.PlayOutAsync(instanceId).ConfigureAwait(true));
                var rowId = Str(args, 0); var sourceId = Str(args, 1);
                if (rowId.Length == 0) return ControlInvokeResult.Fail("graphics.playOut needs rowId (plus sourceId for a guest entry) or instanceId.");
                if (_vm.GraphicsPlayout.Row(rowId) is not { } row) return ControlInvokeResult.Fail("That playlist row does not exist.");
                if ((row.Kind == GraphicsPlaylistRow.Guest || row.HasVariants) && sourceId.Length == 0)
                {
                    // "Hide guest" / "Hide names list" with no source: every entry of that row on Program goes out.
                    var onAir = _vm.GraphicsStrip.OnProgram.Where(i => i.RowId == rowId).Select(i => i.InstanceId).ToArray();
                    if (onAir.Length == 0) return ControlInvokeResult.Fail($"{row.Name} is not on Program.");
                    NativeGraphicsOperationResult last = new() { Accepted = true };
                    foreach (var id in onAir) { last = await _vm.GraphicsPlayout.PlayOutAsync(id).ConfigureAwait(true); if (!last.Accepted) break; }
                    return Graphics(last);
                }
                return Graphics(await _vm.GraphicsPlayout.PlayOutAsync(GraphicsPlayoutService.InstanceIdFor(rowId, row.Kind == GraphicsPlaylistRow.Guest ? sourceId : "")).ConfigureAwait(true));
            }
            case "graphics.preview.remove":
                return Graphics(await _vm.GraphicsPlayout.RemoveFromPreviewAsync(Str(args, 0)).ConfigureAwait(true));
            case "graphics.playOutAll":
                return Graphics(await _vm.GraphicsPlayout.PlayOutAllAsync().ConfigureAwait(true));
            case "graphics.preview.clear":
                return Graphics(await _vm.GraphicsPlayout.ClearPreviewAsync().ConfigureAwait(true));
            case "graphics.strip":
                return ControlInvokeResult.WithData(JsonSerializer.Serialize(ControlStrip(), PlayoutJson));

            // ---- Shot layouts (#1076 slice 2) ---------------------------------------
            case "graphics.layout.list":
                await _vm.GraphicsLibrary.LoadAsync().ConfigureAwait(true);
                return ControlInvokeResult.WithData(LayoutsJson());
            case "graphics.layout.set":
            {
                ShotLayout? layout;
                try { layout = JsonSerializer.Deserialize<ShotLayout>(Str(args, 0), PlayoutJson); }
                catch (JsonException ex) { return ControlInvokeResult.Fail($"layoutJson is not a shot layout: {ex.Message}"); }
                if (layout is null || !layout.IsValid) return ControlInvokeResult.Fail("layoutJson is not a valid shot layout (id a-z0-9-, name, sceneId or blank, rowIds, hotkey Shift+F1..Shift+F11 or blank, playOutOthers).");
                await _vm.GraphicsLibrary.LoadAsync().ConfigureAwait(true);
                if (layout.SceneId.Length > 0 && _vm.Scenes.All(scene => scene.Id != layout.SceneId))
                    return ControlInvokeResult.Fail($"Scene '{layout.SceneId}' is not in this show. Use one of: {string.Join(", ", _vm.Scenes.Select(scene => scene.Id))}.");
                var missing = layout.RowIds.Where(id => _vm.GraphicsPlayout.Row(id) is null).ToArray();
                if (missing.Length > 0) return ControlInvokeResult.Fail($"Playlist rows do not exist: {string.Join(", ", missing)}.");
                var layouts = _vm.GraphicsPlayout.Layouts.ToList();
                // One key, one layout: a key already assigned elsewhere moves here.
                if (layout.Hotkey.Length > 0) layouts = layouts.Select(l => l.Id != layout.Id && l.Hotkey == layout.Hotkey ? l with { Hotkey = "" } : l).ToList();
                var index = layouts.FindIndex(l => l.Id == layout.Id);
                if (index < 0) layouts.Add(layout); else layouts[index] = layout;
                return await SaveLayouts(layouts).ConfigureAwait(true);
            }
            case "graphics.layout.remove":
            {
                await _vm.GraphicsLibrary.LoadAsync().ConfigureAwait(true);
                var layouts = _vm.GraphicsPlayout.Layouts.ToList();
                if (layouts.RemoveAll(l => l.Id == Str(args, 0)) == 0) return ControlInvokeResult.Fail("That shot layout does not exist.");
                return await SaveLayouts(layouts).ConfigureAwait(true);
            }
            case "graphics.layout.cue":
                return Graphics(await _vm.CueLayoutAsync(Str(args, 0)).ConfigureAwait(true));
            case "graphics.layout.take":
                return Graphics(await _vm.CueAndTakeLayoutAsync(Str(args, 0)).ConfigureAwait(true));
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
            case "audio.zoomMode.set":
            {
                var mode = Str(args, 0).Trim();
                if (string.Equals(mode, ZoomAudioModePreference.PerGuestIsoValue, StringComparison.OrdinalIgnoreCase))
                {
                    _vm.SetZoomAudioMode(perGuestIso: true);
                    return ControlInvokeResult.Success;
                }
                if (string.Equals(mode, ZoomAudioModePreference.ProgramMixValue, StringComparison.OrdinalIgnoreCase))
                {
                    _vm.SetZoomAudioMode(perGuestIso: false);
                    return ControlInvokeResult.Success;
                }
                return ControlInvokeResult.Fail("mode must be 'perGuestIso' or 'programMix'.");
            }
            case "audio.monitor.set":
                return await ApplyAudioMonitorControlAsync(args, enabled: Bool(args, 0));
            case "audio.monitor.volume":
                return await ApplyAudioMonitorControlAsync(args, volume: Math.Clamp(Double(args, 0), 0.0, 1.0));
            case "audio.route.set":
                return await ApplyAudioRouteControlAsync(args);
            case "audio.masterLimiter.set":
                _vm.MasterLimiterEnabled = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "audio.mastering.set":
                _vm.MasteringEnabled = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "audio.mastering.target":
                _vm.MasteringTargetIndex = Math.Clamp(Int(args, 0), 0, 2);
                return ControlInvokeResult.Success;
            case "audio.vst.scan":
                _vm.RequestVstPluginScan();
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
            case "automation.magic":
                return RunMagicScene(_vm.RunMagicSceneCommand);
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
                // A playlist Guest row means lower thirds are native instances: arm the native
                // rules, never the legacy MagicScene key (#1076 C1).
                if(_vm.ManagedLowerThirdAuthority || _vm.HasPlaylistGuestRow) {
                    var result=await _vm.ApplyManagedGraphicsAutomationAsync(Bool(args,0));
                    return result.Accepted?ControlInvokeResult.Success:ControlInvokeResult.Fail(result.Reason);
                }
                _vm.AutomationLowerThirdsEnabled = Bool(args, 0);
                return ControlInvokeResult.Success;
            case "automation.captions.set":
                _vm.AutomationCaptionsEnabled = Bool(args, 0);
                return ControlInvokeResult.Success;

            // ---- Browser sources (BR-1) ----------------------------------------------
            case "browser.add":
            {
                var url = Str(args, 0);
                if (url.Length == 0)
                {
                    return ControlInvokeResult.Fail("browser.add requires a url argument.");
                }
                _vm.NewBrowserSourceUrl = url;
                // Optional width/height select the closest preset (BR-1 exposes the
                // three preset sizes; BR-2 grows a full custom-size dialog).
                if (args.Count > 2 && args[1] is int w && args[2] is int h && w > 0 && h > 0)
                {
                    _vm.NewBrowserSourcePresetIndex = (w, h) switch
                    {
                        (1280, 720) => 1,
                        (960, 540) => 2,
                        _ => 0
                    };
                }
                await _vm.AddBrowserSourceCommand.ExecuteAsync(null).ConfigureAwait(true);
                return ControlInvokeResult.Success;
            }
            case "browser.remove":
                try
                {
                    await _vm.RemoveBrowserSourceAsync(Str(args, 0)).ConfigureAwait(true);
                    return ControlInvokeResult.Success;
                }
                catch (Exception ex)
                {
                    return ControlInvokeResult.Fail(ex.Message);
                }
            case "browser.reload":
                try
                {
                    await _vm.ReloadBrowserSourceAsync(Str(args, 0)).ConfigureAwait(true);
                    return ControlInvokeResult.Success;
                }
                catch (Exception ex)
                {
                    return ControlInvokeResult.Fail(ex.Message);
                }

            default:
                return ControlInvokeResult.Fail($"Action '{actionId}' is not implemented by the WinUI surface.");
        }
    }

    // ---- Graphics playout helpers (#1076) ---------------------------------------------------
    private static readonly JsonSerializerOptions PlayoutJson = new(JsonSerializerDefaults.Web);

    private static ControlInvokeResult Graphics(NativeGraphicsOperationResult result) =>
        result.Accepted ? ControlInvokeResult.Success : ControlInvokeResult.Fail(result.Reason);

    private async Task<ControlInvokeResult> SavePlaylist(IReadOnlyList<GraphicsPlaylistRow> rows) =>
        await _vm.GraphicsLibrary.SavePlaylistAsync(rows).ConfigureAwait(true) ? ControlInvokeResult.Success : ControlInvokeResult.Fail(_vm.GraphicsLibrary.Status);
    private async Task<ControlInvokeResult> SaveLayouts(IReadOnlyList<ShotLayout> layouts) =>
        await _vm.GraphicsLibrary.SaveShotLayoutsAsync(layouts).ConfigureAwait(true) ? ControlInvokeResult.Success : ControlInvokeResult.Fail(_vm.GraphicsLibrary.Status);

    private IReadOnlyList<ControlGraphicsLayout> ControlLayouts() =>
        _vm.GraphicsPlayout.Layouts.Select(l => new ControlGraphicsLayout(l.Id, l.Name, l.SceneId, l.Hotkey)).ToArray();

    /// <summary>#1070: the scene rail as data, so a drill can verify a catalog id still exists
    /// before cueing it (the outputs drill cues `intro` / `speaker-slides` by id). The hidden
    /// multiview-solo scenes are filtered (SceneCatalogPolicy.SceneListJson).</summary>
    private string ScenesJson() => SceneCatalogPolicy.SceneListJson(_vm.Scenes, _vm.PreviewSceneId, _vm.ActiveSceneId);

    private string LayoutsJson() => JsonSerializer.Serialize(new
    {
        layouts = _vm.GraphicsPlayout.Layouts.Select(l => new
        {
            id = l.Id, name = l.Name, sceneId = l.SceneId, rowIds = l.RowIds, hotkey = l.Hotkey, playOutOthers = l.PlayOutOthers,
            sceneExists = l.SceneId.Length == 0 || _vm.Scenes.Any(scene => scene.Id == l.SceneId),
        }),
        scenes = _vm.Scenes.Select(scene => new { id = scene.Id, name = scene.Name }),
        previewSceneId = _vm.PreviewSceneId, activeSceneId = _vm.ActiveSceneId,
    }, PlayoutJson);

    /// <summary>rowId (+ optional sourceId) -> playable entry. Guest rows need a bound source: the
    /// given one, else the current Program subject. Other rows ignore sourceId.</summary>
    private ControlInvokeResult? ResolvePlaylistEntry(string rowId, string sourceId, out GraphicsPlaylistEntry? entry)
    {
        entry = null;
        if (_vm.GraphicsPlayout.Row(rowId) is not { } row) return ControlInvokeResult.Fail(rowId.Length == 0 ? "rowId is required." : $"Playlist row '{rowId}' does not exist.");
        if (row.HasVariants)
        {
            // Names list: a named variant, else the next one after whatever is on Program.
            entry = sourceId.Length > 0 ? _vm.GuestEntryFor(sourceId) : GraphicsPlayoutService.NextVariant(row, _vm.GraphicsStrip.OnProgram.Where(i => i.RowId == rowId).Select(i => i.SourceId));
            return entry is null ? ControlInvokeResult.Fail($"'{sourceId}' is not a line of {row.Name}'s names list.") : null;
        }
        if (row.Kind != GraphicsPlaylistRow.Guest) { entry = GraphicsPlayoutService.EntryFor(row); return null; }
        if (sourceId.Length > 0)
        {
            entry = _vm.GuestEntryFor(sourceId);
            return entry is null ? ControlInvokeResult.Fail($"'{sourceId}' is not bound to a Show Input, so it has no guest entry.") : null;
        }
        entry = _vm.ProgramSubjectGuestEntry;
        return entry is null ? ControlInvokeResult.Fail("No Program subject is bound to a Show Input. Pass sourceId to choose a guest.") : null;
    }

    private ControlGraphicsStrip ControlStrip()
    {
        var strip = _vm.GraphicsStrip;
        static ControlGraphicsStripItem Chip(GraphicsStripItem item) => new(item.InstanceId, item.RowId, item.SourceId, item.Label, item.Owner, item.Visibility);
        return new ControlGraphicsStrip
        {
            OnProgram = strip.OnProgram.Select(Chip).ToArray(),
            OnPreview = strip.OnPreview.Select(Chip).ToArray(),
            Auto = strip.Auto.Select(Chip).ToArray(),
            AutomationMode = strip.AutomationMode,
            AutomationArmed = strip.AutomationArmed,
        };
    }

    private IReadOnlyList<ControlGraphicsPlaylistRow> ControlPlaylist() =>
        _vm.GraphicsPlayout.Playlist.Select(r => new ControlGraphicsPlaylistRow(r.Id, r.Kind, r.Name, r.Hotkey, r.PlayOutAfterMs)).ToArray();

    private string PlaylistJson() => JsonSerializer.Serialize(new
    {
        rows = _vm.GraphicsPlayout.Playlist.Select(r => new
        {
            id = r.Id, kind = r.Kind, name = r.Name, hotkey = r.Hotkey, playOutAfterMs = r.PlayOutAfterMs,
            templateId = r.TemplateId, templateVersion = r.TemplateVersion, followSources = r.FollowSources, showClock = r.ShowClock,
            hasImage = r.Image is not null, instanceId = r.Kind == GraphicsPlaylistRow.Guest ? null : GraphicsPlayoutService.InstanceIdFor(r.Id, ""),
        }),
        guests = _vm.GuestEntries.Select(e => new { rowId = e.RowId, sourceId = e.SourceId, label = e.Label, title = e.Title, instanceId = GraphicsPlayoutService.InstanceIdFor(e) }),
        programSubjectSourceId = _vm.ProgramLowerThirdSubjectSourceId,
    }, PlayoutJson);

    private ControlInvokeResult CuePreviewWithRoutes(string sceneId, IReadOnlyDictionary<string, int?> routes)
    {
        if (!_vm.TryGetSceneRouteIds(sceneId, out var routeIds))
        {
            return ControlInvokeResult.Fail($"Unknown scene '{sceneId}'.");
        }

        var knownScenes = _vm.Scenes.Select(scene => scene.Id).ToList();
        var slots = _vm.ShowInputs.Select(input => input.SlotNumber).ToList();
        var error = SceneCueWithRoutesArgs.Refuse(sceneId, knownScenes, routeIds, slots, routes);
        if (error is not null)
        {
            return ControlInvokeResult.Fail(error);
        }

        _vm.SelectSceneCommand.Execute(sceneId);
        if (!string.Equals(_vm.PreviewSceneId, sceneId, StringComparison.Ordinal))
        {
            return ControlInvokeResult.Fail($"Scene '{sceneId}' was not queued on Preview.");
        }

        var missing = _vm.SetPreviewRouteSlots(routes);
        if (missing.Count > 0)
        {
            return ControlInvokeResult.Fail($"Unknown route '{missing[0]}' on scene '{sceneId}'.");
        }

        return ControlInvokeResult.AppliedPreview(_vm.PreviewSceneId);
    }

    private ControlInvokeResult AssignInput(int slot, string sourceId)
    {
        if (!TryGetEditor(slot, out var editor))
        {
            return ControlInvokeResult.Fail($"Show Input slot {slot} is out of range (1-{_vm.ShowInputEditors.Count}).");
        }

        // Operator-equivalent path (Companion/control API) — named so slot-write logs
        // distinguish it from an in-app picker action.
        using var _ = ShowInputWriteScope.Enter("control-api.assign");

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
        return device is null ? ShowInputKind.UvcWebcam : ShowInputRosterService.InferCaptureDeviceKind(device);
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
        var inputNames = inputs
            .Where(input => !string.IsNullOrWhiteSpace(input.SourceId))
            .GroupBy(input => input.SourceId!, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First().Name, StringComparer.Ordinal);
        var audioMix = _vm.AudioMix;
        var appliedMonitor = _vm.AppliedAudioMonitorSession;
        var audioSources = audioMix.Participants
            .Where(source => !string.IsNullOrWhiteSpace(source.ParticipantId))
            .OrderBy(source => source.ParticipantId, StringComparer.Ordinal)
            .Select(source => new ControlAudioSourceState(
                source.ParticipantId,
                inputNames.GetValueOrDefault(source.ParticipantId) ??
                    inputNames.GetValueOrDefault($"zoom:{source.ParticipantId}") ??
                    source.ParticipantId,
                source.OutputLevel,
                source.TruePeakDb,
                source.Lufs,
                source.Muted || source.SourceMuted,
                source.Status))
            .ToList();

        var state = NativeControlEvidence.Apply(new ControlState
        {
            ProgramBufferRequestedFrames = _vm.ProgramBufferFrames,
            ProgramBufferSessionRequestedFrames = _vm.ProgramBufferSessionRequestedFrames,
            ProgramBufferRestartRequired = _vm.ProgramBufferRestartRequired,
            Recording = _vm.Recording,
            Streaming = _vm.Streaming,
            EngineOn = _vm.ZoomCaptureSubscribed,
            CanTake = _vm.CanTake,
            // Under instances authority the strip's lower-third chip is the evidence (#1076 C2);
            // NativeLowerThirdVisible below stays the legacy key's own observation.
            LowerThirdOnAir = _vm.ManagedLowerThirdAuthority ? _vm.GraphicsLowerThirdOnAir : _vm.ProgramLowerThirdKey.IsVisible,
            ZoomStatus = _vm.ZoomStatus,
            EngineStatus = _vm.EngineStatus,
            CommandStatus = _vm.CommandStatus,
            ActiveSceneId = _vm.ActiveSceneId,
            PreviewSceneId = _vm.PreviewSceneId,
            PreviewTilesNamesOn = _vm.PreviewScene.DynamicGallery?.NameEachTile ?? false,
            ViewMode = _vm.ViewMode.ToString(),
            MultiviewLayoutMode = _vm.MultiviewLayoutMode,
            MultiviewTileCount = _vm.MultiviewTileCount,
            AutomationOn = _vm.ProductionMode != ProductionMode.Manual,
            AutoTake = _vm.AutomationAutoTakeEnabled,
            AutoAssignInputs = _vm.AutomationAutoAssignInputsEnabled,
            AutoLowerThirds = _vm.AutomationLowerThirdsEnabled,
            AutoCaptions = _vm.AutomationCaptionsEnabled,
            AudioMonitorOn = appliedMonitor?.MonitorEnabled ?? false,
            AudioMonitorVolume = appliedMonitor?.MonitorVolume ?? 0,
            AudioMonitorAuthorityEpoch = appliedMonitor?.MonitorControl?.AuthorityEpoch ?? string.Empty,
            AudioMonitorRevision = appliedMonitor?.MonitorControl?.Revision ?? 0,
            AudioMonitorLastResult = appliedMonitor?.MonitorControl?.LastResult?.Status ?? string.Empty,
            AudioRouteAuthorityEpoch = _vm.NativeControlSnapshot?.AudioRoutingMatrix.Control?.AuthorityEpoch ?? string.Empty,
            AudioRouteRevision = _vm.NativeControlSnapshot?.AudioRoutingMatrix.Control?.Revision ?? 0,
            AudioRouteLastResult = _vm.NativeControlSnapshot?.AudioRoutingMatrix.Control?.LastResult?.Status ?? string.Empty,
            AudioRoutes = _vm.NativeControlSnapshot?.AudioRoutingMatrix.Sends
                .Select(send => new ControlAudioRouteState(send.SourceId, send.BusId, send.GainDb)).ToArray() ?? [],
            ControlRecoverySummary = _vm.ControlRecoverySummary,
            ZoomRosterEpoch = _vm.NativeControlSnapshot?.RosterEpoch ?? string.Empty,
            ZoomRosterRevision = _vm.NativeControlSnapshot?.RosterRevision ?? 0,
            ZoomParticipantIds = _vm.RoomParticipantsForInputs.Select(participant => participant.Id).ToArray(),
            ZoomAudioMode = ZoomAudioModePreference.Format(_vm.ZoomAudioMode),
            MasterLimiterOn = _vm.MasterLimiterEnabled,
            MasteringOn = _vm.MasteringEnabled,
            MasteringTarget = _vm.MasteringTargetIndex,
            AudioSourceCount = audioSources.Count,
            ProgramTruePeakDbfs = _vm.MasteringMeterTruePeakDbfs,
            ProgramLoudnessLufs = audioMix.LoudnessLufs,
            AudioValidationSummary = _vm.AudioValidationSummary,
            VstHostStatus = _vm.VstHostStatus,
            VstPluginCount = _vm.VstPlugins.Count,
            VstHostSummary = _vm.VstPluginHostSummary,
            NdiOutputOn = _vm.NdiOutputEnabled,
            NdiOutputStatus = _vm.NdiOutputStatusLabel,
            NdiGraphicsKeyOn = _vm.StreamNdiGraphicsKeyEnabled,
            NdiGraphicsKeyName = StudioStreamOutputValidation.ResolveNdiGraphicsKeyName(_vm.StreamNdiProgramName, _vm.StreamNdiGraphicsName),
            VirtualCameraOn = _vm.VirtualCameraEnabled,
            VirtualCameraStatus = _vm.VirtualCameraStatusLabel,
            VirtualCameraRawStatus = _vm.VirtualCameraRawStatus,
            Inputs = inputs,
            AudioSources = audioSources,
            GraphicsStrip = ControlStrip(),
            GraphicsPlaylist = ControlPlaylist(),
            GraphicsLayouts = ControlLayouts(),
        }, _vm.NativeControlSnapshot);

        return WithOhg(state, _bridge?.Latest, _bridge?.Health ?? StoppedHealth, _ohgAdapterSlot.Current?.ShadowLastCommand);
    }

    /// <summary>Serves the media core's own snapshot to read-only transports (GET /snapshot).
    ///
    /// This is a plain read of the reference the bridge already publishes on its existing sync
    /// cadence — no core round-trip, no UI-thread marshal, no lock the render path takes. The
    /// snapshot record is immutable, so handing out the reference cannot race a publication: a
    /// concurrent publish swaps in a new record and this caller keeps the one it read.
    ///
    /// Redaction is not optional here — <see cref="CoreSnapshotObserver.Observe"/> is the only
    /// way the raw text reaches the control layer.</summary>
    public NativeSnapshotObservation GetNativeSnapshot()
    {
        var observation = CoreSnapshotObserver.Observe(_vm.NativeControlSnapshot);
        return new NativeSnapshotObservation(
            observation.Json,
            observation.ReceivedUtc,
            observation.UnavailableReason);
    }

    private static readonly ShowEngineHealth StoppedHealth =
        new(ShowEngineState.Stopped, Generation: 0, RestartCount: 0, LastError: null, LastCrashAt: null);

    /// <summary>Spec §7's state node, as a pure projection so it is unit-testable without a
    /// DispatcherQueue. `Ohg`/`OhgFields` stay NULL until the engine has published a snapshot —
    /// an absent node is honest about "nothing to report", where an empty object would read as
    /// "a show with nothing in it". `OhgEngineHealth` is ALWAYS present.</summary>
    public static ControlState WithOhg(
        ControlState baseState,
        ShowEngineSnapshot? latest,
        ShowEngineHealth health,
        string? shadowLast)
        => baseState with
        {
            Ohg = latest?.Snapshot,
            OhgFields = latest?.Fields,
            OhgEngineHealth = health.State.ToString().ToLowerInvariant(),
            OhgShadowLastCommand = shadowLast ?? string.Empty,
        };

    // ---- OHG show engine (Plan 7a Task 11, spec §3/§7/§8) ---------------------------

    internal const string RestartActionId = "showEngine.restart";
    internal const string NoShowEngine = "OHG show engine is not configured";

    /// <summary>The shell action behind the workspace's Restart button. Marshal-free like the
    /// `ohg.*` forwards — the supervisor owns its own threading, and a restart must work even
    /// when the UI thread is the thing that is wedged.</summary>
    private async Task<ControlInvokeResult> RestartShowEngineAsync(CancellationToken cancellationToken)
    {
        if (_bridge is null)
        {
            return ControlInvokeResult.Fail(NoShowEngine);
        }

        try
        {
            await _bridge.RestartAsync(cancellationToken).ConfigureAwait(false);
            return ControlInvokeResult.Success;
        }
        catch (Exception ex)
        {
            // RestartAsync throws when the engine was never started (nothing to restart) — a
            // legitimate refusal, not a crash.
            return ControlInvokeResult.Fail(ex.Message);
        }
    }

    // Reader thread -> the EXISTING 150 ms feedback debounce, so an engine ticking at 4 Hz
    // coalesces into the same push rate as every other feedback source.
    private void OnBridgeSnapshotChanged(object? sender, ShowEngineSnapshot snapshot) => ScheduleFeedbackPush();

    private void OnBridgeHealthChanged(object? sender, ShowEngineHealth health) => ScheduleFeedbackPush();

    private void ScheduleFeedbackPush() => UiDispatch.Run(_dispatcher, () =>
    {
        if (_disposed)
        {
            return;
        }

        _feedbackTimer.Stop();
        _feedbackTimer.Start();
    }, "control-surface.ohg-feedback");

    // Host commands DO touch the ViewModel, so they marshal (spec §3). ApplyAsync never throws
    // by contract; the wrapper below is belt-and-braces because this body runs inside a queued
    // dispatcher callback.
    //
    // AND THEY RUN ONE AT A TIME, IN SEQ ORDER. Marshaling alone only orders the STARTS: `cut` and
    // `auto` await TakeAsync, and the next command's `applyLook` would otherwise run inside that
    // await and rewrite the preview draft mid-take — putting the wrong guest on air. The queue
    // holds each command until its predecessor has fully finished, awaits included.
    private void OnBridgeHostCommand(object? sender, ShowEngineHostCommand command)
    {
        var binding = _ohgAdapterSlot.Capture();
        UiDispatch.Run(_dispatcher,
            () => _ = _ohgCommands.Enqueue(() => ApplyHostCommandAsync(command, binding)),
            "control-surface.ohg-host-command");
    }
    /// <summary>Serializes <see cref="ApplyHostCommandAsync"/> across awaits. Enqueued from inside
    /// the UiDispatch callback above, so every link runs on the UI thread.</summary>
    private readonly SequentialAsyncQueue _ohgCommands = new(
        ex => LaunchLog.Write($"ohg: host command queue link failed :: {ex}"));

    private async Task ApplyHostCommandAsync(ShowEngineHostCommand command, OhgAdapterSlot.Binding binding)
    {
        // Reject commands queued before a settings swap or engine restart.
        var adapter = _ohgAdapterSlot.Resolve(binding, command.Generation, _bridge?.Health.Generation ?? -1);
        if (_disposed || adapter is null)
        {
            return;
        }

        try
        {
            var refusal = await adapter.ApplyAsync(command).ConfigureAwait(true);
            if (_disposed || _ohgAdapterSlot.Resolve(binding, command.Generation, _bridge?.Health.Generation ?? -1) is null) return;
            if (refusal is { Length: > 0 })
            {
                // Operator-visible: a refused host command must never be silent.
                _vm.CommandStatus = refusal;

                // ...and on the OHG tab's own status strip (spec §10). The operator producing the
                // show is looking at that strip, not at the workspace-wide command status line.
                _vm.OhgShow?.NoteAdapterRefusal(refusal);
            }

            // Shadow mode's whole product value is SEEING what the engine would have done, so the
            // adapter's last shadowed line is pushed to the workspace after EVERY apply — a
            // refusal included (the adapter records what it was asked to do either way).
            if (adapter.ShadowLastCommand is { Length: > 0 } shadowed)
            {
                _vm.OhgShow?.SetShadowLastCommand(shadowed);
            }
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"ohg: host command '{command?.Name}' threw :: {ex}");
        }
    }

    /// <summary>UI-thread settings swap: retire queued payloads and reject commands
    /// from the old engine even between the swap and the subsequent restart.</summary>
    public void ReplaceOhgAdapter(OhgHostAdapter? adapter)
        => _ohgAdapterSlot.Replace(adapter, (long)(_bridge?.Health.Generation ?? 0) + 1);
    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_disposed || _feedbackTimer is null || e.PropertyName is null || !FeedbackProps.Contains(e.PropertyName))
        {
            return;
        }

        // Debounce on the UI thread — coalesce a burst of property changes into one push.
        _feedbackTimer.Stop();
        _feedbackTimer.Start();
    }

    // Transport toggle/set that reports HONESTLY: when the underlying command is
    // gated off (e.g. record needs a meeting), it returns Fail with the reason
    // instead of the old false ok:true (the "record.set says ok but recording
    // stays false" bug). A set whose target already matches is a Success no-op.
    private static async Task<ControlInvokeResult> RunTransportToggle(
        CommunityToolkit.Mvvm.Input.IAsyncRelayCommand command, string what)
    {
        if (!command.CanExecute(null))
        {
            return ControlInvokeResult.Fail($"Cannot toggle {what} right now (the control is unavailable in the current state).");
        }
        await command.ExecuteAsync(null).ConfigureAwait(true);
        return ControlInvokeResult.Success;
    }

    internal static ControlInvokeResult RunMagicScene(System.Windows.Input.ICommand command)
    {
        if (!command.CanExecute(null)) return ControlInvokeResult.Fail("Magic Scene is unavailable in the current state.");
        try
        {
            command.Execute(null);
            return ControlInvokeResult.Success;
        }
        catch (Exception ex) { return ControlInvokeResult.Fail($"Magic Scene failed: {ex.Message}"); }
    }

    internal static async Task<ControlInvokeResult> RunTake(bool canTake, Func<Task<TakeResult>> take)
    {
        if (!canTake)
            return ControlInvokeResult.Fail("Take is unavailable. Queue a valid Preview scene or a changed draft and wait for any current Take to finish.");
        try
        {
            var result = await take().ConfigureAwait(true);
            return result.Succeeded ? ControlInvokeResult.Success
                : ControlInvokeResult.Fail(result.Error ?? "Take was not confirmed by the media core.");
        }
        catch (Exception ex)
        {
            return ControlInvokeResult.Fail($"Take failed: {ex.Message}");
        }
    }

    internal static async Task<ControlInvokeResult> RunOutputSet(
        bool requested, bool observedLive, bool target, Func<bool, bool> canSet,
        Func<bool, Task> set, string what)
    {
        // A disarmed intent is not proof that a previous Stop reached the core.
        // Retry the explicit target while media is still live; never invert intent here.
        if (requested == target && (target || !observedLive))
        {
            return ControlInvokeResult.Success;
        }
        if (!canSet(target))
        {
            return ControlInvokeResult.Fail($"Cannot set {what} right now (the control is unavailable in the current state).");
        }
        await set(target).ConfigureAwait(true);
        return ControlInvokeResult.Success;
    }

    private static async Task<ControlInvokeResult> RunTransportSet(
        bool current, bool target, CommunityToolkit.Mvvm.Input.IAsyncRelayCommand command, string what)
    {
        if (current == target)
        {
            return ControlInvokeResult.Success;  // already in the requested state
        }
        if (!command.CanExecute(null))
        {
            return ControlInvokeResult.Fail(
                $"Cannot {(target ? "start" : "stop")} {what} right now (the control is unavailable in the current state).");
        }
        await command.ExecuteAsync(null).ConfigureAwait(true);
        return ControlInvokeResult.Success;
    }

    // R6 (final review, #535 slice 4a): the pure refusal decision for
    // "source.dropout.set", extracted so it is unit-testable without
    // constructing a StudioViewModel (which the WinUI test project cannot
    // do). Returns the Fail result to return verbatim, or null when the id
    // is a "zoom:<pid>" id and the caller should proceed.
    internal static ControlInvokeResult? ValidateZoomOnlyDropoutSourceId(string? sourceId) =>
        string.IsNullOrWhiteSpace(sourceId) || !sourceId.StartsWith("zoom:", StringComparison.Ordinal)
            ? ControlInvokeResult.Fail(
                $"source.dropout.set: only zoom:<pid> sources take a dropout policy this slice ({sourceId})")
            : null;

    // Success confirms accepted intent, not completed animation/native pixels.
    // The VM can reject a toggle (e.g. no eligible program source); propagate
    // that outcome instead of making API clients wait for an impossible key.
    internal static ControlInvokeResult ApplyLowerThirdIntent(bool requested, bool forceToggle,
        Func<bool> readIntent, Action toggle, Func<string> readStatus)
    {
        if (forceToggle || readIntent() != requested) toggle();
        if (readIntent() == requested) return ControlInvokeResult.Success;
        var reason = readStatus();
        return ControlInvokeResult.Fail(string.IsNullOrWhiteSpace(reason)
            ? "Lower third request was not accepted." : reason);
    }

    private static string Str(IReadOnlyList<object?> args, int index) => args.Count > index && args[index] is string s ? s : string.Empty;

    private static bool Bool(IReadOnlyList<object?> args, int index) => args.Count > index && args[index] is bool b && b;

    public static ControlInvokeResult ApplyProgramBufferRequest(object? requested, Action<int> save)
    {
        // Use the Double wire schema: the registry's Int coercion rounds fractions.
        // Validate again here so direct callers cannot bypass the exact 2|3 contract.
        double? frames = requested switch { int i => i, long l => l, double d => d, float f => f, _ => null };
        if (frames is not (2 or 3)) return ControlInvokeResult.Fail("Program buffer frames must be exactly 2 or 3. Changes require an app restart.");
        try
        {
            save((int)frames.Value);
            return ControlInvokeResult.Success;
        }
        catch (Exception ex) { return ControlInvokeResult.Fail("Program buffer setting was not saved: " + ex.Message); }
    }

    public static ControlInvokeResult ApplyTilesSlotRequest(double slot, string sourceId, Action<int, string> assign)
    {
        if (!double.IsFinite(slot) || slot != Math.Truncate(slot) || slot is < 1 or > 64)
            return ControlInvokeResult.Fail("Tiles slot must be an integer from 1 to 64.");
        try { assign((int)slot, sourceId); return ControlInvokeResult.Success; }
        catch (Exception ex) { return ControlInvokeResult.Fail(ex.Message); }
    }

    private static int Int(IReadOnlyList<object?> args, int index) => args.Count > index && args[index] is int i ? i : 0;

    private static double Double(IReadOnlyList<object?> args, int index) => args.Count > index && args[index] is double d ? d : 0.0;

    private async Task<ControlInvokeResult> ApplyAudioMonitorControlAsync(
        IReadOnlyList<object?> args, bool? enabled = null, double? volume = null)
    {
        var epoch = Str(args, 1);
        var revision = args.Count > 2 && args[2] is double value && double.IsFinite(value) &&
                       value >= 0 && value <= 9007199254740991d && value == Math.Truncate(value)
            ? (long?)value : null;
        if (args.Count > 2 && args[2] is not null && revision is null)
            return ControlInvokeResult.Fail("expectedRevision must be a nonnegative safe integer.");
        var operationId = Str(args, 3);
        var result = await _vm.RequestAudioMonitorControlAsync(
            enabled, volume, string.IsNullOrWhiteSpace(epoch) ? null : epoch,
            revision, string.IsNullOrWhiteSpace(operationId) ? null : operationId).ConfigureAwait(true);
        return result.Kind == AudioMonitorControlOutcomeKind.Applied
            ? ControlInvokeResult.Success
            : ControlInvokeResult.Fail(result.Kind == AudioMonitorControlOutcomeKind.Conflict
                ? $"Monitor control conflict at revision {result.Control?.Revision}; read state and retry with the current epoch and revision."
                : $"Monitor control {result.Kind.ToString().ToLowerInvariant()}; application is unconfirmed.");
    }

    private async Task<ControlInvokeResult> ApplyAudioRouteControlAsync(IReadOnlyList<object?> args)
    {
        var sourceId = Str(args, 0).Trim();
        var busId = Str(args, 1).Trim();
        var gainDb = Double(args, 3);
        var epoch = Str(args, 4).Trim();
        var operationId = Str(args, 6).Trim();
        if (sourceId.Length == 0 || busId.Length == 0 || epoch.Length == 0 || operationId.Length == 0)
            return ControlInvokeResult.Fail("sourceId, busId, authorityEpoch and operationId are required.");
        if (gainDb < -60 || gainDb > 10 || !double.IsFinite(gainDb))
            return ControlInvokeResult.Fail("gainDb must be between -60 and +10.");
        if (args[5] is not double revision || !double.IsFinite(revision) || revision < 0 ||
            revision > 9007199254740991d || revision != Math.Truncate(revision))
            return ControlInvokeResult.Fail("expectedRevision must be a nonnegative safe integer.");
        var draft = new CoreVideoPro.MediaCore.Models.MediaCoreAudioRouteWire(
            sourceId, busId, Bool(args, 2), gainDb);
        var result = await _vm.RequestAudioRouteControlAsync(
            draft, epoch, (long)revision, operationId).ConfigureAwait(true);
        return result.Kind == AudioRouteControlOutcomeKind.Applied
            ? ControlInvokeResult.Success
            : ControlInvokeResult.Fail(result.Kind == AudioRouteControlOutcomeKind.Conflict
                ? $"Audio route conflict at revision {result.Control?.Revision}; read state and retry."
                : $"Audio route {result.Kind.ToString().ToLowerInvariant()}; application is unconfirmed.");
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _vm.PropertyChanged -= OnViewModelPropertyChanged;
        if (_bridge is not null)
        {
            _bridge.SnapshotChanged -= OnBridgeSnapshotChanged;
            _bridge.HealthChanged -= OnBridgeHealthChanged;
            _bridge.HostCommand -= OnBridgeHostCommand;
        }

        _feedbackTimer.Stop();
    }
}
