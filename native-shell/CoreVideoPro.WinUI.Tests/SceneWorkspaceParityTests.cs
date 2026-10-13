using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class SceneWorkspaceParityTests
{
    [Fact]
    public void InclusionAndNeverShowHaveSeparatePersistedIntent()
    {
        var wall=new DynamicGallerySettings{MembershipMode="routed",OmittedSourceIds=["zoom:1"],ExcludedSourceIds=["zoom:2"]};
        Assert.Equal(["zoom:3"],TilesMembershipPolicy.Resolve(wall,["zoom:1","zoom:2","zoom:3"],["zoom:1","zoom:2","zoom:3"]));
        wall.MembershipMode="eligible";
        Assert.Equal(["zoom:1","zoom:3"],TilesMembershipPolicy.Resolve(wall,["zoom:1","zoom:2","zoom:3"]));
        var restored=ScenePersistenceService.FromPersisted(ScenePersistenceService.ToPersisted(wall));
        Assert.Equal(wall.OmittedSourceIds,restored.OmittedSourceIds);
        restored.OmittedSourceIds.Clear();Assert.Single(wall.OmittedSourceIds);
    }
    [Fact]
    public void CapacityDoesNotSilentlyUncheckConfiguredMembers()
    {
        var wall=new DynamicGallerySettings{MembershipMode="eligible",MaxTiles=1};
        var states=SceneWorkspaceMembershipPolicy.Build(wall,["zoom:1","zoom:2"],new HashSet<string>(),[],null);
        Assert.All(states.Values,state=>Assert.True(state.Selected));Assert.All(states.Values,state=>Assert.False(state.CanSelect));
        wall.MembershipMode="routed";var routed=new HashSet<string>{"zoom:1"};
        states=SceneWorkspaceMembershipPolicy.Build(wall,["zoom:1","zoom:2"],routed,[],null);
        Assert.True(states["zoom:1"].Selected);Assert.False(states["zoom:2"].CanSelect);
    }
    [Fact]
    public void AuthoredGraphicSurvivesCloneAndSceneRoundTrip()
    {
        var route=new SourceRoute{Id="title",WorkspaceAuthored=true,Graphic=new("text","Guest","#8AE0CA"),ParticipantId="media:revision-one",DisplayName="Guest title"};
        var clone=route.Clone();clone.Graphic=clone.Graphic! with{Text="Changed"};
        Assert.Equal("Guest",route.Graphic!.Text);
        var restored=ScenePersistenceService.FromPersisted(ScenePersistenceService.ToPersisted(route));
        Assert.Equal(route.Graphic,restored.Graphic);Assert.Equal(route.ParticipantId,restored.ParticipantId);
        SceneRoutingService.ApplyNormalizeRouteUpdate(restored,[]);Assert.Equal(route.Graphic,restored.Graphic);
    }
    [Fact]
    public void SmallAuthoredGraphicsRetainTheirPixelGeometryAfterSave()
    {
        var route=new SourceRoute{Id="small-logo",WorkspaceAuthored=true,CanvasRect=new(){X=.2,Y=.3,Width=24d/1920,Height=24d/1080}};
        var loaded=ScenePersistenceService.FromPersisted(ScenePersistenceService.ToPersisted(route));
        Assert.Equal(route.CanvasRect.Width,loaded.CanvasRect!.Width);Assert.Equal(route.CanvasRect.Height,loaded.CanvasRect.Height);
        route.WorkspaceAuthored=false;loaded=ScenePersistenceService.FromPersisted(ScenePersistenceService.ToPersisted(route));
        Assert.Equal(.08,loaded.CanvasRect!.Width);
    }
    [Fact]
    public void GraphicRastersAreRealPixelsAndImmutableAcrossEdits()
    {
        var root=Path.Combine(Path.GetTempPath(),"CoreVideoGraphicTest-"+Guid.NewGuid().ToString("N"));
        try
        {
            var red=new SceneGraphic("color",Color:"#FF0000");
            var first=SceneGraphicRasterizer.Materialize(red,root);
            var file=Assert.Single(Directory.GetFiles(Path.Combine(root,"image")));
            var original=File.ReadAllBytes(file);
            using(var bitmap=new System.Drawing.Bitmap(file)){var pixel=bitmap.GetPixel(0,0);Assert.Equal(255,pixel.R);Assert.Equal(0,pixel.G);Assert.Equal(255,pixel.A);}
            Assert.Equal(first,SceneGraphicRasterizer.Materialize(red,root));
            SceneGraphicRasterizer.Materialize(red with{Color="#0000FF"},root);
            Assert.Equal(original,File.ReadAllBytes(file));
            var before=Directory.GetFiles(Path.Combine(root,"image"));
            SceneGraphicRasterizer.Materialize(new("text","TITLE"),root);
            var title=Assert.Single(Directory.GetFiles(Path.Combine(root,"image")).Except(before));
            using var text=new System.Drawing.Bitmap(title);Assert.Equal(1920,text.Width);Assert.Equal(216,text.Height);
            Assert.Equal(0,text.GetPixel(0,0).A);
            var opaque=0;for(var y=0;y<text.Height;y+=4)for(var x=0;x<text.Width;x+=4)if(text.GetPixel(x,y).A>0)opaque++;
            Assert.True(opaque>100,"Authored text has no glyph pixels");
        }
        finally
        {
            // Only individually delete files created by this test in its unique root.
            if(Directory.Exists(Path.Combine(root,"image")))foreach(var file in Directory.GetFiles(Path.Combine(root,"image")))File.Delete(file);
            if(Directory.Exists(Path.Combine(root,"image")))Directory.Delete(Path.Combine(root,"image"));
            if(Directory.Exists(root))Directory.Delete(root);
        }
    }
}
