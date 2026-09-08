using System.Collections.Generic;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Plan 7b Task 2 — <see cref="OhgActionArgs"/> builders against the 28-action contract in
/// <c>show-engine/src/actions.ts</c>'s <c>OHG_ACTIONS</c>. The id list and per-action param
/// types below are copied here as LITERALS deliberately: this is the contract, and drift between
/// this table and the TS source is a red test on purpose (see the class remarks in
/// <see cref="OhgActionArgs"/>).
/// </summary>
public sealed class OhgActionArgsTests
{
    private const string T_STRING = "string";
    private const string T_INT = "int";
    private const string T_BOOL = "bool";

    /// <summary>The 28 <c>ohg.*</c> action ids, verbatim from <c>OHG_DISPATCHED_ACTION_IDS</c> /
    /// <c>OHG_ACTIONS</c> in <c>show-engine/src/actions.ts</c>.</summary>
    private static readonly HashSet<string> AllActionIds = new()
    {
        "ohg.panelist.add",
        "ohg.panelist.remove",
        "ohg.panelist.replace",
        "ohg.panelist.role.set",
        "ohg.panelist.syncAll",
        "ohg.program.preview",
        "ohg.program.cut",
        "ohg.program.auto",
        "ohg.program.directCut",
        "ohg.program.asFollow.set",
        "ohg.look.set",
        "ohg.look.nextGuest",
        "ohg.look.prevGuest",
        "ohg.look.box.assign",
        "ohg.look.box.clear",
        "ohg.gallery.resetFromSlots",
        "ohg.gallery.replace",
        "ohg.gallery.remove",
        "ohg.gallery.empty",
        "ohg.gallery.smart.set",
        "ohg.gfx.headline.in",
        "ohg.gfx.headline.out",
        "ohg.gfx.headline.change",
        "ohg.gfx.question.in",
        "ohg.gfx.question.out",
        "ohg.mukana.sync",
        "ohg.mukana.override.set",
        "ohg.mukana.override.delete",
    };

    /// <summary>
    /// The literal (id, paramTypes[]) contract table — required-params-only counts, i.e. the
    /// count/types every invocation (other than <c>ohg.panelist.add</c>'s optional trailing
    /// slot) must match exactly.
    /// </summary>
    private static readonly Dictionary<string, string[]> ContractParamTypes = new()
    {
        ["ohg.panelist.add"] = new[] { T_STRING }, // + optional trailing int (slot)
        ["ohg.panelist.remove"] = new[] { T_INT },
        ["ohg.panelist.replace"] = new[] { T_INT, T_STRING },
        ["ohg.panelist.role.set"] = new[] { T_STRING, T_STRING },
        ["ohg.panelist.syncAll"] = System.Array.Empty<string>(),
        ["ohg.program.preview"] = new[] { T_STRING },
        ["ohg.program.cut"] = System.Array.Empty<string>(),
        ["ohg.program.auto"] = System.Array.Empty<string>(),
        ["ohg.program.directCut"] = new[] { T_STRING },
        ["ohg.program.asFollow.set"] = new[] { T_BOOL },
        ["ohg.look.set"] = new[] { T_STRING },
        ["ohg.look.nextGuest"] = System.Array.Empty<string>(),
        ["ohg.look.prevGuest"] = System.Array.Empty<string>(),
        ["ohg.look.box.assign"] = new[] { T_INT, T_INT },
        ["ohg.look.box.clear"] = new[] { T_INT },
        ["ohg.gallery.resetFromSlots"] = System.Array.Empty<string>(),
        ["ohg.gallery.replace"] = new[] { T_INT, T_INT },
        ["ohg.gallery.remove"] = new[] { T_INT },
        ["ohg.gallery.empty"] = System.Array.Empty<string>(),
        ["ohg.gallery.smart.set"] = new[] { T_BOOL },
        ["ohg.gfx.headline.in"] = System.Array.Empty<string>(),
        ["ohg.gfx.headline.out"] = System.Array.Empty<string>(),
        ["ohg.gfx.headline.change"] = new[] { T_STRING, T_STRING },
        ["ohg.gfx.question.in"] = System.Array.Empty<string>(),
        ["ohg.gfx.question.out"] = System.Array.Empty<string>(),
        ["ohg.mukana.sync"] = System.Array.Empty<string>(),
        ["ohg.mukana.override.set"] = new[] { T_STRING, T_STRING, T_STRING, T_STRING },
        ["ohg.mukana.override.delete"] = new[] { T_STRING },
    };

    public static IEnumerable<object[]> AllBuilderInvocations()
    {
        yield return Wrap(OhgActionArgs.PanelistAdd("p1", slot: 3)); // 2-arg add form
        yield return Wrap(OhgActionArgs.PanelistRemove(2));
        yield return Wrap(OhgActionArgs.PanelistReplace(2, "p1"));
        yield return Wrap(OhgActionArgs.PanelistRoleSet("0042", "host"));
        yield return Wrap(OhgActionArgs.PanelistSyncAll());
        yield return Wrap(OhgActionArgs.ProgramPreview("black"));
        yield return Wrap(OhgActionArgs.ProgramCut());
        yield return Wrap(OhgActionArgs.ProgramAuto());
        yield return Wrap(OhgActionArgs.ProgramDirectCut("gallery"));
        yield return Wrap(OhgActionArgs.ProgramAsFollowSet(true));
        yield return Wrap(OhgActionArgs.LookSet("look-1"));
        yield return Wrap(OhgActionArgs.LookNextGuest());
        yield return Wrap(OhgActionArgs.LookPrevGuest());
        yield return Wrap(OhgActionArgs.LookBoxAssign(1, 2));
        yield return Wrap(OhgActionArgs.LookBoxClear(1));
        yield return Wrap(OhgActionArgs.GalleryResetFromSlots());
        yield return Wrap(OhgActionArgs.GalleryReplace(1, 2));
        yield return Wrap(OhgActionArgs.GalleryRemove(1));
        yield return Wrap(OhgActionArgs.GalleryEmpty());
        yield return Wrap(OhgActionArgs.GallerySmartSet(true));
        yield return Wrap(OhgActionArgs.HeadlineIn());
        yield return Wrap(OhgActionArgs.HeadlineOut());
        yield return Wrap(OhgActionArgs.HeadlineChange("Name", "Location"));
        yield return Wrap(OhgActionArgs.QuestionIn());
        yield return Wrap(OhgActionArgs.QuestionOut());
        yield return Wrap(OhgActionArgs.MukanaSync());
        yield return Wrap(OhgActionArgs.OverrideSet("0042", "Name", "Location", "host"));
        yield return Wrap(OhgActionArgs.OverrideDelete("0042"));
    }

    private static object[] Wrap((string ActionId, object?[] Args) invocation)
        => new object[] { invocation.ActionId, invocation.Args };

    [Theory]
    [MemberData(nameof(AllBuilderInvocations))]
    public void EveryBuilderMatchesTheActionContract(string actionId, object?[] args)
    {
        Assert.Contains(actionId, AllActionIds);
        Assert.True(ContractParamTypes.TryGetValue(actionId, out var declaredTypes), $"no contract entry for {actionId}");

        if (actionId == "ohg.panelist.add")
        {
            // Both the 1-arg (slot omitted) and 2-arg (slot given) forms are valid for this
            // action alone.
            Assert.True(args.Length is 1 or 2, $"{actionId}: expected 1 or 2 args, got {args.Length}");
        }
        else
        {
            Assert.Equal(declaredTypes!.Length, args.Length);
        }

        for (var i = 0; i < args.Length; i++)
        {
            var expectedType = i < declaredTypes!.Length ? declaredTypes[i] : T_INT; // add's trailing slot
            AssertArgType(actionId, i, expectedType, args[i]);
        }
    }

    [Fact]
    public void AllTwentyEightActionIdsAreCovered()
    {
        var covered = new HashSet<string>();
        foreach (var invocation in AllBuilderInvocations())
        {
            covered.Add((string)invocation[0]);
        }

        Assert.Equal(28, AllActionIds.Count);
        Assert.Equal(AllActionIds, covered);
    }

    [Fact]
    public void PanelistAddWithNullSlotYieldsExactlyOneArg()
    {
        var (actionId, args) = OhgActionArgs.PanelistAdd("participant-1", slot: null);

        Assert.Equal("ohg.panelist.add", actionId);
        Assert.Single(args);
        Assert.Equal("participant-1", args[0]);
    }

    [Fact]
    public void PanelistAddWithSlotYieldsTwoArgs()
    {
        var (actionId, args) = OhgActionArgs.PanelistAdd("participant-1", slot: 3);

        Assert.Equal("ohg.panelist.add", actionId);
        Assert.Equal(2, args.Length);
        Assert.Equal("participant-1", args[0]);
        Assert.Equal(3, args[1]);
        Assert.IsType<int>(args[1]);
    }

    [Fact]
    public void FourDigitPinStaysAStringNeverCoercedToAnInt()
    {
        var (_, roleArgs) = OhgActionArgs.PanelistRoleSet("0042", "host");
        Assert.IsType<string>(roleArgs[0]);
        Assert.Equal("0042", roleArgs[0]);

        var (_, overrideSetArgs) = OhgActionArgs.OverrideSet("0042", "Name", "Location", "host");
        Assert.IsType<string>(overrideSetArgs[0]);
        Assert.Equal("0042", overrideSetArgs[0]);

        var (_, overrideDeleteArgs) = OhgActionArgs.OverrideDelete("0042");
        Assert.IsType<string>(overrideDeleteArgs[0]);
        Assert.Equal("0042", overrideDeleteArgs[0]);
    }

    [Fact]
    public void SourceForSlotAndSourceForLookProduceTheWireStrings()
    {
        Assert.Equal("slot:3", OhgActionArgs.SourceForSlot(3));
        Assert.Equal("look:abc-123", OhgActionArgs.SourceForLook("abc-123"));
    }

    private static void AssertArgType(string actionId, int index, string expectedType, object? value)
    {
        switch (expectedType)
        {
            case T_INT:
                Assert.True(value is int, $"{actionId} arg[{index}] expected int, was {value?.GetType().Name ?? "null"}");
                break;
            case T_BOOL:
                Assert.True(value is bool, $"{actionId} arg[{index}] expected bool, was {value?.GetType().Name ?? "null"}");
                break;
            case T_STRING:
                Assert.True(value is string, $"{actionId} arg[{index}] expected string, was {value?.GetType().Name ?? "null"}");
                break;
            default:
                Assert.Fail($"unknown expected type '{expectedType}'");
                break;
        }
    }
}
