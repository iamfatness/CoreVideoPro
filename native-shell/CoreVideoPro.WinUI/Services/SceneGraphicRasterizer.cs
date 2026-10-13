using System.Drawing;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Security.Cryptography;
using System.Text;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Rasterize only on authored-content changes, never on the production frame path.
/// Content-addressed files preserve the captured Program revision when Preview changes.</summary>
public static class SceneGraphicRasterizer
{
    public static string Materialize(SceneGraphic graphic,string? mediaRoot=null)
    {
        if(graphic.Kind is not ("text" or "color"))throw new ArgumentException("Choose Text or Color background.");
        if(graphic.Text.Length>4096)throw new ArgumentException("Text must contain at most 4096 characters.");
        var color=ColorTranslator.FromHtml(SceneRoutingService.NormalizeBorderColor(graphic.Color));
        var hash=Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes($"v2|{graphic.Kind}|{graphic.Text}|{color.ToArgb()}"))).ToLowerInvariant();
        var relative=Path.Combine("image",$"scene-graphic-{hash}.png");
        var path=Path.Combine(mediaRoot??MediaBinClassifier.DefaultMediaRoot,relative);
        if(File.Exists(path))return MediaBinClassifier.BuildAssetId(relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var bitmap=new Bitmap(graphic.Kind=="color"?2:1920,graphic.Kind=="color"?2:216,PixelFormat.Format32bppArgb);
        using var canvas=Graphics.FromImage(bitmap);
        canvas.Clear(graphic.Kind=="color"?color:Color.Transparent);
        if(graphic.Kind=="text")
        {
            canvas.TextRenderingHint=TextRenderingHint.AntiAliasGridFit;
            var fontSize=120f;
            using var brush=new SolidBrush(color);
            using var format=new StringFormat {Alignment=StringAlignment.Center,LineAlignment=StringAlignment.Center,FormatFlags=StringFormatFlags.NoWrap};
            using(var measureFont=new Font("Segoe UI",fontSize,FontStyle.Bold,GraphicsUnit.Pixel))
            {
                var measured=canvas.MeasureString(graphic.Text,measureFont,int.MaxValue,format);
                if(measured.Width>bitmap.Width)fontSize=Math.Max(1,fontSize*bitmap.Width/measured.Width);
            }
            using var font=new Font("Segoe UI",fontSize,FontStyle.Bold,GraphicsUnit.Pixel);
            canvas.DrawString(graphic.Text,font,brush,new RectangleF(0,0,bitmap.Width,bitmap.Height),format);
        }
        var temporary=path+"."+Guid.NewGuid().ToString("N")+".tmp";
        try {bitmap.Save(temporary,ImageFormat.Png);File.Move(temporary,path,true);}
        finally {if(File.Exists(temporary))File.Delete(temporary);}
        return MediaBinClassifier.BuildAssetId(relative);
    }
}
