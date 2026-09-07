using System;
using System.Linq;
using CoreVideoPro.Control;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class ControlCatalogTests
{
    [Fact]
    public void StaticOnly_EqualsTheRegistry()
    {
        var catalog = ControlCatalog.StaticOnly;
        Assert.Equal(ControlActionRegistry.Actions.Select(a => a.Id), catalog.Actions.Select(a => a.Id));
        Assert.Equal(OscExposure.Lan, catalog.ExposureOf("transport.take"));
    }

    [Fact]
    public void ProviderActions_AppearAfterStaticOnes_AndBindLikeAnyOther()
    {
        var provider = new FakeActionProvider("show-engine", OscExposure.LoopbackOnly,
            new ControlAction("ohg.panelist.remove", "Remove", "…", new[] { new ControlParam("slot", ControlParamType.Int) }),
            new ControlAction("ohg.program.cut", "Cut", "…"));
        var catalog = new ControlCatalog(new[] { provider });

        Assert.Equal(ControlActionRegistry.Actions.Count + 2, catalog.Actions.Count);
        Assert.Equal("ohg.panelist.remove", catalog.Actions[^2].Id);
        Assert.True(catalog.TryBind("ohg.panelist.remove", new object?[] { "3" }, out var bound, out var error));
        Assert.Null(error);
        Assert.Equal(3, bound[0]);
        Assert.False(catalog.TryBind("ohg.panelist.remove", Array.Empty<object?>(), out _, out var missing));
        Assert.Contains("requires parameter 'slot'", missing);
        Assert.Equal(OscExposure.LoopbackOnly, catalog.ExposureOf("ohg.program.cut"));
        Assert.Equal(OscExposure.Lan, catalog.ExposureOf("transport.take"));
    }

    [Fact]
    public void ProviderMayNotShadowAStaticAction_OrUseAnInvalidId()
    {
        var shadowing = new FakeActionProvider("p", OscExposure.Lan, new ControlAction("transport.take", "x", "y"));
        var catalog = new ControlCatalog(new[] { shadowing });
        Assert.False(catalog.Contains("transport.take") && catalog.Actions.Count(a => a.Id == "transport.take") > 1);
        Assert.Contains("duplicate", catalog.LastValidationError, StringComparison.OrdinalIgnoreCase);

        var badId = new FakeActionProvider("p", OscExposure.Lan, new ControlAction("Ohg.Bad", "x", "y"));
        var catalog2 = new ControlCatalog(new[] { badId });
        Assert.False(catalog2.Contains("Ohg.Bad"));
        Assert.Contains("Ohg.Bad", catalog2.LastValidationError);
    }

    [Fact]
    public void FeedbackFields_AreStaticFieldsThenProviderTemplates()
    {
        var provider = new FakeActionProvider("p", OscExposure.Lan) { FeedbackFieldTemplates = new[] { "ohg/slot/{slot}/name" } };
        var catalog = new ControlCatalog(new[] { provider });
        Assert.Equal(ControlManifest.StateFields.Concat(new[] { "ohg/slot/{slot}/name" }), catalog.FeedbackFields);
    }

    [Fact]
    public void ActionsChanged_OnAProvider_IsVisibleImmediately_AndRaisesChanged()
    {
        var provider = new FakeActionProvider("p", OscExposure.Lan);
        var catalog = new ControlCatalog(new[] { provider });
        var raised = 0;
        catalog.Changed += (_, _) => raised++;
        Assert.False(catalog.Contains("ohg.program.cut"));
        provider.Set(new ControlAction("ohg.program.cut", "Cut", "…"));
        Assert.True(catalog.Contains("ohg.program.cut"));
        Assert.Equal(1, raised);
        provider.Set();   // provider went away
        Assert.False(catalog.Contains("ohg.program.cut"));
        Assert.Equal(2, raised);
    }
}
