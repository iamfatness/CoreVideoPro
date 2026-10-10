namespace CoreVideoPro.MediaCore.Models;

/// <summary>Source-coordinate drawing geometry shared by pointer and precision edits.</summary>
public static class ScopeRoiGeometry
{
    public static (double X,double Y,double Width,double Height) FitImage(double hostWidth,double hostHeight,int sourceWidth,int sourceHeight)
    {
        if(hostWidth<=0 || hostHeight<=0 || sourceWidth<=0 || sourceHeight<=0) return default;
        var scale=Math.Min(hostWidth/sourceWidth,hostHeight/sourceHeight);
        var width=sourceWidth*scale;var height=sourceHeight*scale;
        return ((hostWidth-width)/2,(hostHeight-height)/2,width,height);
    }
    public static GradeScopeRoi Draw(double startX, double startY, double endX, double endY,
        int sourceWidth, int sourceHeight, string shape, long revision = 0)
    {
        if (sourceWidth <= 0 || sourceHeight <= 0) throw new ArgumentOutOfRangeException(nameof(sourceWidth));
        if (!double.IsFinite(startX) || !double.IsFinite(startY) || !double.IsFinite(endX) || !double.IsFinite(endY))
            throw new ArgumentException("ROI coordinates must be finite.");
        if (shape is not ("rectangle" or "circle")) throw new ArgumentException("Unknown ROI shape.", nameof(shape));
        startX=Math.Clamp(startX,0,1); startY=Math.Clamp(startY,0,1);
        endX=Math.Clamp(endX,0,1); endY=Math.Clamp(endY,0,1);
        var x = Math.Min(startX, endX); var y = Math.Min(startY, endY);
        var w = Math.Max(1d / sourceWidth, Math.Abs(endX - startX));
        var h = Math.Max(1d / sourceHeight, Math.Abs(endY - startY));
        if (shape == "circle")
        {
            var diameter = Math.Max(1, Math.Min(w * sourceWidth, h * sourceHeight));
            w = diameter / sourceWidth; h = diameter / sourceHeight;
            x = endX < startX ? startX - w : startX;
            y = endY < startY ? startY - h : startY;
        }
        return new(true, Math.Clamp(x, 0, 1 - w), Math.Clamp(y, 0, 1 - h), w, h, revision, shape);
    }

    public static GradeScopeRoi CircleSize(GradeScopeRoi roi, double diameter, int sourceWidth, int sourceHeight)
    {
        if (!roi.IsValid || !double.IsFinite(diameter) || sourceWidth<=0 || sourceHeight<=0)
            throw new ArgumentException("Invalid circle geometry.");
        roi=roi with { X=Math.Min(roi.X,1-1d/sourceWidth), Y=Math.Min(roi.Y,1-1d/sourceHeight) };
        diameter = Math.Clamp(diameter, 1, Math.Max(1, Math.Min((1 - roi.X) * sourceWidth, (1 - roi.Y) * sourceHeight)));
        return roi with { Width = diameter / sourceWidth, Height = diameter / sourceHeight };
    }
}
