using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Protects an operator's unsent form from observed configuration changes.
/// Runtime trace/arming changes alone never invalidate a rule edit.</summary>
public sealed class GraphicsAutomationEditGuard
{
    private string _epoch = "", _target = "", _mode = "manual";
    private GraphicsAutomationRule? _rule;
    private bool _loaded;
    public bool Dirty { get; private set; }
    public bool Conflict { get; private set; }
    public void MarkEdited() => Dirty = true;

    // True means the form can load the latest observed values. False means keep
    // every field, including an unavailable guest ID, until explicit reload.
    public bool Observe(string epoch, string target, NativeGraphicsAutomation? state)
    {
        var rule = state?.Configuration.Rules.FirstOrDefault(r => r.InstanceId == target);
        var mode = state?.Mode ?? "manual";
        if (_loaded && Dirty)
        {
            Conflict |= epoch != _epoch || target != _target || mode != _mode || rule != _rule;
            return false;
        }
        var changed = !_loaded || epoch != _epoch || target != _target || mode != _mode || rule != _rule;
        _loaded = true; _epoch = epoch; _target = target; _mode = mode; _rule = rule;
        return changed;
    }

    public void Reload() { Dirty = Conflict = false; _loaded = false; }

    public static string ModeHelp(string mode) => mode switch
    {
        "suggest" => "Suggest lists eligible introductions. You choose Cue, then Show; it never reveals one automatically.",
        "rehearsal" => "Rehearsal prepares Preview only after Arm. Program stays unchanged.",
        "automatic" => "Automatic can reveal introductions on Program after Arm and the subject dwell. Hide or Clear takes priority.",
        _ => "Manual leaves Cue, Show and Hide in your hands. No rule runs automatically."
    };
}
