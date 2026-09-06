using System.Text.Json;
using CoreVideoPro.WinUI.Services;
namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private object? _lowerThirdCoreProfile;
    private long _lowerThirdCoreGeneration;
    private readonly RenderedProgramFreshness _lowerThirdFreshness = new();
    private void ObserveLowerThirdProgramIntent()
    {
        if (!ReferenceEquals(_lowerThirdCoreProfile, _bridge.Profile))
        {
            _lowerThirdCoreProfile = _bridge.Profile;
            _lowerThirdCoreGeneration++;
        }
        var sources = GetMutableRoutes(ActiveSceneId).Select(ResolveRouteFromShowInput).Select(route => new
        {
            route.Id, route.Mode, route.ParticipantId, route.CaptureDeviceId,
            route.ShowInputSlotNumber, route.ProductionRoleId
        });
        _lowerThirdFreshness.Observe(_lowerThirdCoreGeneration + "|" + ActiveSceneId + "|" + JsonSerializer.Serialize(sources));
    }
}
