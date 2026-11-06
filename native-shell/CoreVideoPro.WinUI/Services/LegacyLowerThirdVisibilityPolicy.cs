using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class LegacyLowerThirdVisibilityPolicy
{
    public static bool HasEnabledGraphic(IEnumerable<GraphicOverlay> graphics, bool automationSuppressed) =>
        graphics.Any(graphic => graphic.Enabled
            && graphic.Kind.Equals("lower-third", StringComparison.OrdinalIgnoreCase)
            && (!automationSuppressed || !string.Equals(graphic.Id, "automation-lower-third", StringComparison.Ordinal)));
}
