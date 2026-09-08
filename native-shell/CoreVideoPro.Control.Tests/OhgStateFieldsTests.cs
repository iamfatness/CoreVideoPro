using System.Text.Json;
using System.Text.RegularExpressions;
using CoreVideoPro.Control;
using CoreVideoPro.Control.Osc;
using Xunit;

namespace CoreVideoPro.Control.Tests;

/// <summary>Spec §7 agreement test: the ten `ohg/...` field-name templates in
/// show-engine/src/controlState.ts's OHG_FIELD_TEMPLATES are the contract this project's
/// ControlManifest.StateFields + OscFeedback.Encode must honor for every `ohg/*` field they can
/// ever emit. Copied here as literals (not imported) so a drift on either side reds this test on
/// purpose — see the TS file's own doc comment for why the two sides must not share one array.</summary>
public sealed class OhgStateFieldsTests
{
    private static readonly string[] OhgFieldTemplates =
    {
        "ohg/slot/{slot}/name",
        "ohg/slot/{slot}/role",
        "ohg/slot/{slot}/tally",
        "ohg/program/mode",
        "ohg/queue/current",
        "ohg/gallery/smart",
        "ohg/health/mukana",
        "ohg/capabilities/registry/state",
        "ohg/capabilities/handsQueue/state",
        "ohg/capabilities/questionFeed/state"
    };

    private static JsonElement Json(string raw) => JsonDocument.Parse(raw).RootElement.Clone();

    [Fact]
    public void Encode_ProjectsOhgFieldsByJsonValueKind()
    {
        var state = ControlState.Empty with
        {
            OhgFields = new Dictionary<string, JsonElement>
            {
                ["ohg/slot/1/name"] = Json("\"Ada\""),
                ["ohg/slot/1/tally"] = Json("true"),
                ["ohg/program/mode"] = Json("\"cut\""),
                ["ohg/queue/current"] = Json("null"),
                ["ohg/gallery/smart"] = Json("false")
            }
        };

        var map = new OscAddressMap();
        var messages = OscFeedback.Encode(state, map);
        var byAddress = messages.ToDictionary(m => m.Address, m => m.Args);

        Assert.Equal("Ada", byAddress["/cvp/state/ohg/slot/1/name"][0]);
        Assert.Equal(1, byAddress["/cvp/state/ohg/slot/1/tally"][0]);
        Assert.Equal(0, byAddress["/cvp/state/ohg/gallery/smart"][0]);
        Assert.False(byAddress.ContainsKey("/cvp/state/ohg/queue/current"));
        Assert.Equal("stopped", byAddress["/cvp/state/ohg/health/engine"][0]);
    }

    [Fact]
    public void StateFieldsAndEncodedOhgFields_MatchTheShowEngineTemplateContract()
    {
        Assert.Contains("ohg/health/engine", ControlManifest.StateFields);
        Assert.Contains("ohg/shadow/lastCommand", ControlManifest.StateFields);

        var state = ControlState.Empty with
        {
            OhgFields = new Dictionary<string, JsonElement>
            {
                ["ohg/slot/1/name"] = Json("\"Ada\""),
                ["ohg/slot/1/role"] = Json("\"host\""),
                ["ohg/slot/1/tally"] = Json("true"),
                ["ohg/program/mode"] = Json("\"cut\""),
                ["ohg/queue/current"] = Json("\"p-2\""),
                ["ohg/gallery/smart"] = Json("false"),
                ["ohg/health/mukana"] = Json("\"ok\""),
                ["ohg/capabilities/registry/state"] = Json("\"ok\""),
                ["ohg/capabilities/handsQueue/state"] = Json("\"ok\""),
                ["ohg/capabilities/questionFeed/state"] = Json("\"ok\"")
            }
        };

        var map = new OscAddressMap();
        var messages = OscFeedback.Encode(state, map);
        var ohgFieldNames = messages
            .Select(m => m.Address[map.StatePrefix.Length..])
            .Where(f => f.StartsWith("ohg/", StringComparison.Ordinal))
            .ToList();

        Assert.NotEmpty(ohgFieldNames);
        foreach (var field in ohgFieldNames)
        {
            var matchesStateFields = ControlManifest.StateFields.Contains(field);
            var matchesTemplate = OhgFieldTemplates.Any(template =>
            {
                const string placeholder = "SLOTPLACEHOLDER";
                var pattern = "^" + Regex.Escape(template.Replace("{slot}", placeholder)).Replace(placeholder, @"\d+") + "$";
                return Regex.IsMatch(field, pattern);
            });
            Assert.True(matchesStateFields || matchesTemplate, $"'{field}' matches neither StateFields nor an OHG_FIELD_TEMPLATES entry.");
        }
    }
}
