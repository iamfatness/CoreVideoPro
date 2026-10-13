using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>A saved-layout schematic; never renders another production scene or reads GPU pixels.</summary>
public sealed class SceneLayoutThumbnailControl : Canvas
{
    public static readonly DependencyProperty ItemProperty=DependencyProperty.Register(nameof(Item),typeof(SceneDisplayItem),typeof(SceneLayoutThumbnailControl),new PropertyMetadata(null,Changed));
    public SceneDisplayItem? Item {get=>(SceneDisplayItem?)GetValue(ItemProperty);set=>SetValue(ItemProperty,value);}
    public SceneLayoutThumbnailControl(){Width=35;Height=23;Background=new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(255,20,24,30));}
    private static void Changed(DependencyObject sender,DependencyPropertyChangedEventArgs args)=>((SceneLayoutThumbnailControl)sender).Draw();
    private void Draw()
    {
        Children.Clear();if(Item is not {} item)return;
        var rects=item.ThumbnailRects;
        if(item.Scene.DynamicGallery is not null)rects=[new(){X=.03,Y=.04,Width=.45,Height=.43},new(){X=.52,Y=.04,Width=.45,Height=.43},new(){X=.03,Y=.53,Width=.45,Height=.43},new(){X=.52,Y=.53,Width=.45,Height=.43},..rects];
        foreach(var rect in rects)
        {
            var box=new Border{Width=Math.Max(1,rect.Width*35),Height=Math.Max(1,rect.Height*23),BorderThickness=new(1),BorderBrush=new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(255,101,125,135)),Background=new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(255,44,62,76)),CornerRadius=new(1)};
            SetLeft(box,rect.X*35);SetTop(box,rect.Y*23);Children.Add(box);
        }
        ToolTipService.SetToolTip(this,"Saved layout: "+item.LayoutLabel+" (schematic)");
    }
}
