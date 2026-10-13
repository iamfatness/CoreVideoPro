using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels.Transport;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class SceneWorkspacePolicyTests
{
    [Fact]
    public void LegacyTileDetectionDoesNotDiscardAnAuthoredGuestOrLogo()
    {
        Assert.True(SceneWorkspacePolicy.IsGeneratedTileRoute("scene-a","scene-a-tile-42"));
        Assert.False(SceneWorkspacePolicy.IsGeneratedTileRoute("scene-a","logo"));
        Assert.False(SceneWorkspacePolicy.IsGeneratedTileRoute("scene-a","scene-b-tile-42"));
        var wall=new Scene{Id="scene-a",DynamicGallery=new()};
        Assert.False(SceneWorkspacePolicy.IsAuthoredRoute(wall,new(){Id="stale-active-speaker"}));
        Assert.True(SceneWorkspacePolicy.IsAuthoredRoute(wall,new(){Id="logo",WorkspaceAuthored=true}));
        Assert.True(SceneWorkspacePolicy.IsAuthoredRoute(new(){Id="ordinary"},new(){Id="legacy-camera"}));
    }
    [Fact]
    public void WallAndAuthoredSourcesKeepSeparateNativeOrderBands()
    {
        var wall=new DynamicGallerySettings {WallOrder=1};
        var back=new SourceRoute {Id="back",ZIndex=0};var front=new SourceRoute {Id="front",ZIndex=2};
        SceneWorkspacePolicy.NormalizeOrder([front,back],wall);
        Assert.Equal(0,back.ZIndex);Assert.Equal(256,wall.WallOrder);Assert.Equal(512,front.ZIndex);
        Assert.True(front.ZIndex > wall.WallOrder+128);
    }
    [Fact]
    public void WallStateRoundTripsAndClonesWithoutAliasingLiveSettings()
    {
        var wall=new DynamicGallerySettings { WallRect=new(){X=.2,Y=.1,Width=.6,Height=.7},WallOrder=256,
            WallOpacity=.625,WallVisible=false,WallLocked=true,TransparentBackground=true };
        var scene=new Scene{Id="custom-wall",Name="Panel",DynamicGallery=wall};
        var route=new SourceRoute{Id="logo",DisplayName="Show logo",Visible=false,EditorLocked=true,Opacity=0,ZIndex=512};
        var preferences=new ProductionOutputPreferences{CustomScenes=[ScenePersistenceService.ToPersisted(scene,[route])]};
        var saved=Assert.Single(ProductionOutputPreferencesSerializer.Deserialize(ProductionOutputPreferencesSerializer.Serialize(preferences))!.CustomScenes);
        var restored=ScenePersistenceService.SceneFromPersisted(saved).DynamicGallery!;
        Assert.Equal(.625,restored.WallOpacity);Assert.False(restored.WallVisible);Assert.True(restored.WallLocked);Assert.True(restored.TransparentBackground);
        Assert.Equal(.6,restored.WallRect.Width);Assert.Equal(256,restored.WallOrder);
        restored.WallRect.Width=.3;Assert.Equal(.6,wall.WallRect.Width);
        var item=ScenePersistenceService.FromPersisted(Assert.Single(saved.Routes));
        Assert.Equal("Show logo",item.DisplayName);Assert.False(item.Visible);Assert.True(item.EditorLocked);Assert.Equal(0,item.Opacity);
        Assert.False(SceneTakeRules.HasPendingChanges([route],[route.Clone()]));
        var edit=route.Clone();edit.Visible=true;Assert.True(SceneTakeRules.HasPendingChanges([route],[edit]));
    }
    [Fact]
    public void AHiddenWallKeepsMembershipButCarriesZeroNativeOpacity()
    {
        var scene=new Scene{Id="panel",DynamicGallery=new(){WallVisible=false,WallOrder=512,WallRect=new(){X=.1,Width=.8}}};
        var payload=TilesLayerPayloadBuilder.Build(scene,[new Participant{Id="42",Name="Guest",Health=FeedHealth.Live}])!;
        Assert.Equal(["zoom:42"],payload.Members);Assert.Equal(0,payload.Opacity);Assert.Equal(512,payload.Order);Assert.Equal(.8,payload.Rect!.Width);
    }
}
