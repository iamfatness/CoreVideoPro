using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

// Owned under the supervisor gate. A recovered child keeps the same app-session
// setting; changing the persisted preference must not alter a running production.
internal sealed class ProgramBufferStartupSettings
{
    private int _frames = ProgramBufferPreference.DefaultFrames;
    private bool _frozen;

    public void Configure(int frames)
    {
        if (_frozen) throw new InvalidOperationException("Program buffer changes require an app restart.");
        _frames = ProgramBufferPreference.Normalize(frames);
    }

    public void ApplyTo(IDictionary<string, string> environment)
    {
        _frozen = true;
        environment[ProgramBufferPreference.EnvironmentVariable] = _frames == 2 ? "2" : "3";
    }
}
