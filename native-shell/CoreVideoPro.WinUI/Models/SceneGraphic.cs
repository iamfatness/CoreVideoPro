namespace CoreVideoPro.WinUI.Models;

/// <summary>Authored graphic intent; immutable raster revisions use the existing native still-media path.</summary>
public sealed record SceneGraphic(string Kind, string Text = "TITLE", string Color = "#FFFFFF");
