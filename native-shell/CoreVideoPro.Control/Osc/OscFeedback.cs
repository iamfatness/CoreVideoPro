namespace CoreVideoPro.Control.Osc;

/// <summary>Encodes a <see cref="ControlState"/> into the OSC feedback messages a control surface
/// polls/subscribes to for button lighting. Booleans are sent as int 0/1 (broadest Companion
/// compatibility); the address space mirrors <see cref="ControlState"/> field names under
/// <c>/cvp/state/</c>.</summary>
public static class OscFeedback
{
    public static IReadOnlyList<OscMessage> Encode(ControlState state, OscAddressMap addressMap)
    {
        OscMessage Flag(string field, bool on) => new(addressMap.StateAddress(field), on ? 1 : 0);
        OscMessage Str(string field, string value) => new(addressMap.StateAddress(field), value ?? string.Empty);

        var messages = new List<OscMessage>
        {
            Flag("recording", state.Recording),
            Flag("streaming", state.Streaming),
            Flag("engineOn", state.EngineOn),
            Flag("canTake", state.CanTake),
            Flag("lowerThirdOnAir", state.LowerThirdOnAir),
            Str("zoomStatus", state.ZoomStatus),
            Str("engineStatus", state.EngineStatus),
            Str("commandStatus", state.CommandStatus),
            Str("activeScene", state.ActiveSceneId),
            Str("previewScene", state.PreviewSceneId),
            Str("viewMode", state.ViewMode),
            Str("multiviewLayout", state.MultiviewLayoutMode),
            new(addressMap.StateAddress("multiviewTileCount"), state.MultiviewTileCount),
            Flag("automationOn", state.AutomationOn),
            Flag("autoTake", state.AutoTake),
            Flag("autoAssignInputs", state.AutoAssignInputs),
            Flag("autoLowerThirds", state.AutoLowerThirds),
            Flag("autoCaptions", state.AutoCaptions),
            Flag("audioMonitorOn", state.AudioMonitorOn),
            new(addressMap.StateAddress("audioMonitorVolume"), (float)state.AudioMonitorVolume),
            Flag("masterLimiterOn", state.MasterLimiterOn),
        };

        foreach (var input in state.Inputs)
        {
            messages.Add(new OscMessage(addressMap.StateAddress($"input/{input.Slot}/inShow"), input.InShow ? 1 : 0));
            messages.Add(new OscMessage(addressMap.StateAddress($"input/{input.Slot}/kind"), input.Kind ?? string.Empty));
            messages.Add(new OscMessage(addressMap.StateAddress($"input/{input.Slot}/source"), input.SourceId ?? string.Empty));
            messages.Add(new OscMessage(addressMap.StateAddress($"input/{input.Slot}/name"), input.Name ?? string.Empty));
        }

        return messages;
    }
}
