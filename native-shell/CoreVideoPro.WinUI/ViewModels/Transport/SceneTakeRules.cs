using System.Text.Json;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels.Transport;

public static class SceneTakeRules
{
    // Route order, source binding, canvas/framing and effects all affect Program.
    // Comparing only media IDs stranded ordinary source and geometry drafts.
    public static bool HasPendingChanges(IReadOnlyList<SourceRoute> program, IReadOnlyList<SourceRoute> preview) =>
        !string.Equals(JsonSerializer.Serialize(program), JsonSerializer.Serialize(preview), StringComparison.Ordinal);
}
