using System.Text.Json;
using System.Text.RegularExpressions;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Result of a legacy-Isadora import (Plan 7b Task 11): the resulting edit model plus a
/// plain-words account of what was found and what wasn't. <see cref="Found"/>/<see cref="NotFound"/>
/// are the whole honesty story here — the importer never guesses a value it could not locate.</summary>
public sealed record IsadoraImportResult(OhgConfigEditModel Model, IReadOnlyList<string> Found, IReadOnlyList<string> NotFound);

/// <summary>One-shot, PURE text importer for the two legacy Isadora config files (controller
/// ruling D4, `docs/superpowers/plans/2026-09-07-show-engine-winui-workspace.md`). Both files are
/// read as raw TEXT — they are JS object literals, not JSON, and no samples exist in this repo —
/// so every extraction is a tolerant regex probe, never a parse that could throw on a legacy
/// syntax quirk (the `"oscRole:"` trailing-colon typo included).</summary>
public static class IsadoraConfigImporter
{
    // "https://hoka.pxclabs.com/phpsdk/php-panel-rest.php?event=officehours&req=panelists" shape.
    // Single OR double quotes; the URL itself has no quotes/whitespace, so [^"'\s]+ up to the
    // literal "?event=" boundary is exactly the base URL the spec calls for.
    private static readonly Regex MukanaUrlPattern = new(
        @"https?://[^""'\s]+php-panel-rest\.php\?event=([A-Za-z0-9_-]+)",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    // Quotes around the key are optional (bare-identifier JS object literal keys are common in the
    // legacy files, e.g. `videoPins: 8` alongside `"videoPins": 8`).
    private static readonly Regex VideoPinsPattern = new(
        @"[""']?videoPins[""']?\s*:\s*(\d+)",
        RegexOptions.Compiled);

    private static readonly Regex TallyUrlLiteralPattern = new(
        @"https?://[^""'\s]*oh\.tally[^""'\s]*",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    private static readonly Regex TallyUrlFieldPattern = new(
        @"[""']tallyUrl[""']\s*:\s*[""']([^""']+)[""']",
        RegexOptions.Compiled);

    /// <summary>Pure. <paramref name="infrastructureJs"/> and <paramref name="mukanaJs"/> are the
    /// raw file texts (either may be null/empty). Starts from a DEEP COPY of
    /// <paramref name="existing"/> (never mutates the caller's model) or <see cref="OhgConfigEditModel.Default"/>.</summary>
    public static IsadoraImportResult Import(string? infrastructureJs, string? mukanaJs, OhgConfigEditModel? existing = null)
    {
        var model = CloneOrDefault(existing);
        var found = new List<string>();
        var notFound = new List<string>();

        ImportMukana(mukanaJs, model, found, notFound);
        ImportCapacity(infrastructureJs, found, notFound);
        ImportTallyUrl(infrastructureJs, mukanaJs, model, found, notFound);
        ImportLooks(existing, model, found);

        return new IsadoraImportResult(model, found, notFound);
    }

    private static void ImportMukana(string? mukanaJs, OhgConfigEditModel model, List<string> found, List<string> notFound)
    {
        var match = string.IsNullOrEmpty(mukanaJs) ? null : MukanaUrlPattern.Match(mukanaJs);
        if (match is { Success: true })
        {
            var fullUrl = match.Value;
            var queryIndex = fullUrl.IndexOf('?');
            var baseUrl = queryIndex >= 0 ? fullUrl[..queryIndex] : fullUrl;
            var evt = match.Groups[1].Value;

            model.MukanaBaseUrl = baseUrl;
            model.MukanaEvent = evt;
            model.RegistryEnabled = true;
            model.HandsQueueEnabled = true;
            model.QuestionFeedEnabled = true;

            found.Add($"Mukana: {baseUrl} event={evt}");
        }
        else
        {
            notFound.Add("Mukana REST URL (php-panel-rest.php?event=…) not found");
        }
    }

    private static void ImportCapacity(string? infrastructureJs, List<string> found, List<string> notFound)
    {
        var match = string.IsNullOrEmpty(infrastructureJs) ? null : VideoPinsPattern.Match(infrastructureJs);
        if (match is { Success: true })
        {
            found.Add($"legacy videoPins = {match.Groups[1].Value}; capacity stays 10 (Show Inputs)");
        }
        else
        {
            notFound.Add("ZoomISO videoPins not found");
        }
    }

    private static void ImportTallyUrl(string? infrastructureJs, string? mukanaJs, OhgConfigEditModel model, List<string> found, List<string> notFound)
    {
        var url = FindTallyUrl(infrastructureJs) ?? FindTallyUrl(mukanaJs);
        if (url is not null)
        {
            model.TallyUrl = url;
            found.Add($"tally URL: {url}");
        }
        else
        {
            notFound.Add("tally URL not found");
        }
    }

    private static string? FindTallyUrl(string? text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return null;
        }

        var literal = TallyUrlLiteralPattern.Match(text);
        if (literal.Success)
        {
            return literal.Value;
        }

        var field = TallyUrlFieldPattern.Match(text);
        return field.Success ? field.Groups[1].Value : null;
    }

    private static void ImportLooks(OhgConfigEditModel? existing, OhgConfigEditModel model, List<string> found)
    {
        if (existing?.Looks is { Count: > 0 } existingLooks)
        {
            found.Add($"looks: kept {existingLooks.Count} existing");
        }
        else
        {
            found.Add("looks: wrote the 4 default looks (pick their scenes)");
        }
    }

    /// <summary>Deep-copies <paramref name="existing"/> (never the caller's instance) by round
    /// tripping it through the same JSON shape <see cref="OhgConfigEditModel.ToConfig"/>/
    /// <see cref="OhgConfigEditModel.FromConfig"/> already use, or starts fresh from
    /// <see cref="OhgConfigEditModel.Default"/> when there is nothing to import onto.</summary>
    private static OhgConfigEditModel CloneOrDefault(OhgConfigEditModel? existing)
    {
        if (existing is null)
        {
            return OhgConfigEditModel.Default();
        }

        var config = existing.ToConfig();
        return OhgConfigEditModel.FromConfig(config, out _);
    }
}
