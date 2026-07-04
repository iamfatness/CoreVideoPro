using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Fixed production-role registry (scenes redesign R1). Roles are assigned to
/// participants per session (deliberately not persisted — rosters change every
/// show) and scene routes can target a ROLE instead of a person, so a saved
/// "Host + Reader" scene works with whoever holds those roles today. The
/// assigned role also rides the participant wire to the core, where the
/// director/automation can key on it.
/// </summary>
public static class ProductionRoleService
{
    /// <summary>Add-source / picker option prefix, e.g. "role:host".</summary>
    public const string OptionPrefix = "role:";

    public static IReadOnlyList<RouteSelectOption> Roles { get; } =
    [
        new() { Value = "host", Label = "Host" },
        new() { Value = "co-host", Label = "Co-host" },
        new() { Value = "reader", Label = "Reader" },
        new() { Value = "panelist", Label = "Panelist" },
        new() { Value = "guest-1", Label = "Guest 1" },
        new() { Value = "guest-2", Label = "Guest 2" },
        new() { Value = "guest-3", Label = "Guest 3" },
        new() { Value = "guest-4", Label = "Guest 4" }
    ];

    /// <summary>Roles plus a leading "no role" entry for assignment dropdowns.</summary>
    public static IReadOnlyList<RouteSelectOption> AssignmentOptions { get; } =
        new List<RouteSelectOption> { new() { Value = "", Label = "No role" } }
            .Concat(Roles)
            .ToList();

    public static bool IsRoleOption(string? optionValue) =>
        optionValue is not null &&
        optionValue.StartsWith(OptionPrefix, StringComparison.OrdinalIgnoreCase);

    public static string? RoleIdFromOption(string? optionValue) =>
        IsRoleOption(optionValue) ? optionValue![OptionPrefix.Length..] : null;

    public static string ToOptionValue(string roleId) => OptionPrefix + roleId;

    public static string RoleLabel(string? roleId) =>
        Roles.FirstOrDefault(role => string.Equals(role.Value, roleId, StringComparison.OrdinalIgnoreCase))?.Label
        ?? roleId ?? string.Empty;
}
