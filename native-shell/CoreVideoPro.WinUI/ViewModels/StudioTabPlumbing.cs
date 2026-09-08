using System;
using System.Collections.Generic;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// The workspace's tab plumbing, as PURE data and functions.
///
/// It lives outside <see cref="StudioViewModel"/> for a concrete reason, not tidiness:
/// <see cref="StudioViewModel"/> holds static <c>TabChrome</c> fields whose initializers construct
/// <c>SolidColorBrush</c>es, so touching ANY static member of that class from a unit test runs its
/// whole static initializer and throws a COMException outside a XAML runtime. Everything here is
/// reachable from a test.
/// </summary>
internal static class StudioTabPlumbing
{
    /// <summary>
    /// The nav key -> tab mapping. An unrecognized key — including null/empty and a differently
    /// cased one — lands on Studio: a nav button with a typo'd parameter must never leave the
    /// workspace on a tab whose page host is not in the visual tree.
    /// </summary>
    internal static StudioTab ParseTab(string? tab) => tab switch
    {
        "settings" => StudioTab.Settings,
        "sources" or "scenes" => StudioTab.Sources,
        "inputs" => StudioTab.Inputs,
        "routing" => StudioTab.Routing,
        "overlays" => StudioTab.Overlays,
        "audio" => StudioTab.Audio,
        "media" => StudioTab.Media,
        "automation" => StudioTab.Automation,
        "ohgshow" => StudioTab.OhgShow,
        _ => StudioTab.Studio
    };

    /// <summary>
    /// Every property whose value is a function of <c>ActiveTab</c>, i.e. everything
    /// <c>OnActiveTabChanged</c> must raise. It is DERIVED from the enum rather than hand-written,
    /// because the failure mode of a missing entry is silent and confusing: the nav button
    /// highlights nothing and the page host never becomes visible, so the tab "does nothing".
    /// </summary>
    internal static readonly IReadOnlyList<string> ActiveTabDependentProperties = Build();

    private static IReadOnlyList<string> Build()
    {
        var names = new List<string> { "ActiveTabKey" };
        foreach (var tab in Enum.GetValues<StudioTab>())
        {
            names.Add($"Is{tab}Tab");
            names.Add($"{tab}TabChrome");
        }

        return names;
    }
}
