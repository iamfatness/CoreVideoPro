using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsDesignerControl
{
    private static string ContentSignature(JsonElement node)
    {
        var properties = new[] { "stroke", "strokeWidth", "diagonal", "outline", "shadow", "minFontSize", "minWidth", "maxWidth", "speed", "gap", "direction", "mode" };
        return string.Join('|', properties.Select(name => ReadProperty(node, name).ToString())) + IsAutoWidth(node);
    }

    private static JsonElement ReadProperty(JsonElement node, string property) =>
        node.ValueKind == JsonValueKind.Object && node.TryGetProperty(property, out var value) ? value : default;

    private static double ReadNumber(JsonElement node, string property, double fallback) =>
        ReadProperty(node, property) is { ValueKind: JsonValueKind.Number } number ? number.GetDouble() : fallback;

    private static string ReadString(JsonElement node, string property, string fallback) =>
        ReadProperty(node, property) is { ValueKind: JsonValueKind.String } value ? value.GetString() ?? fallback : fallback;

    private static bool IsAutoWidth(JsonElement node) => ReadString(node, "width", "") == "auto";

    private void AddContentControls(GraphicsLayerInspection layer, JsonElement node)
    {
        if (layer.Kind is "Shape" or "Ellipse" or "Line") BuildStrokeControls(layer, node);
        if (layer.Kind is "Shape" or "Group") BuildSizingControls(layer, node);
        if (layer.Kind is "Text" or "Clock") BuildTextEffects(layer, node);
    }

    private void BuildStrokeControls(GraphicsLayerInspection layer, JsonElement node)
    {
        var stroke = ContentText("Stroke colour", ReadString(node, "stroke", "#FFFFFF"));
        var width = ContentNumber("Stroke width", layer.Kind == "Line" ? 1 : 0, 4096,
            ReadNumber(node, "strokeWidth", layer.Kind == "Line" ? 4 : 0));
        var panel = ContentOptions(stroke, width);
        ComboBox? direction = null;
        if (layer.Kind == "Line")
        {
            direction = new ComboBox { Header = "Diagonal", ItemsSource = new[] { "Down", "Up" },
                SelectedIndex = ReadString(node, "diagonal", "down") == "up" ? 1 : 0 };
            _inspectorInputs.Add(direction);
            panel.Children.Add(direction);
        }
        AddContentApply(panel, "Apply stroke", () => Apply(s => GraphicsTemplateEditor.SetStroke(s, layer.Id,
            stroke.Text, width.Value, direction?.SelectedIndex == 1 ? "up" : "down")));
        _layerBody.Children.Add(new Expander { Header = "Shape stroke", Content = panel, HorizontalAlignment = HorizontalAlignment.Stretch });
    }

    private void BuildSizingControls(GraphicsLayerInspection layer, JsonElement node)
    {
        var enabled = new CheckBox { Content = "Size to content", IsChecked = IsAutoWidth(node) };
        _inspectorInputs.Add(enabled);
        var minimum = ContentNumber("Minimum width", 1, 8192, ReadNumber(node, "minWidth", 80));
        var maximum = ContentNumber("Maximum width", 1, 8192, ReadNumber(node, "maxWidth", Math.Max(80, layer.Width)));
        var panel = ContentOptions(enabled, minimum, maximum);
        AddContentApply(panel, "Apply width", () =>
        {
            var measured = _session?.AuthoringLayout?.Nodes.FirstOrDefault(item => item.Id == layer.Id)?.Width;
            Apply(s => GraphicsTemplateEditor.SetAutoWidth(s, layer.Id, enabled.IsChecked == true, minimum.Value, maximum.Value,
                Math.Clamp(measured ?? (layer.Width > 0 ? layer.Width : minimum.Value), 1, 8192)));
        });
        _layerBody.Children.Add(new Expander { Header = "Content width", Content = panel, HorizontalAlignment = HorizontalAlignment.Stretch });
    }

    private void BuildTextEffects(GraphicsLayerInspection layer, JsonElement node)
    {
        var outline = ReadProperty(node, "outline");
        var shadow = ReadProperty(node, "shadow");
        var minimum = ContentNumber("Minimum font size (0 keeps size)", 0, Math.Max(1, layer.FontSize), ReadNumber(node, "minFontSize", 0));
        var outlineColour = ContentText("Outline colour", ReadString(outline, "color", "#000000"));
        var outlineWidth = ContentNumber("Outline width", 0, 8, ReadNumber(outline, "width", 0));
        var shadowEnabled = new CheckBox { Content = "Drop shadow", IsChecked = shadow.ValueKind == JsonValueKind.Object };
        _inspectorInputs.Add(shadowEnabled);
        var shadowColour = ContentText("Shadow colour", ReadString(shadow, "color", "#00000080"));
        var shadowX = ContentNumber("Shadow X", -256, 256, ReadNumber(shadow, "x", 2));
        var shadowY = ContentNumber("Shadow Y", -256, 256, ReadNumber(shadow, "y", 4));
        var blur = ContentNumber("Shadow blur", 0, 16, ReadNumber(shadow, "blur", 4));
        var panel = ContentOptions(minimum, outlineColour, outlineWidth, shadowEnabled, shadowColour, shadowX, shadowY, blur);
        AddContentApply(panel, "Apply text effects", () => Apply(s => GraphicsTemplateEditor.SetTextEffects(s, layer.Id,
            minimum.Value, outlineColour.Text, outlineWidth.Value, shadowEnabled.IsChecked == true ? shadowColour.Text : null,
            shadowX.Value, shadowY.Value, blur.Value)));
        _layerBody.Children.Add(new Expander { Header = "Text effects", Content = panel, HorizontalAlignment = HorizontalAlignment.Stretch });
    }

    private static StackPanel ContentOptions(params UIElement[] elements)
    {
        var panel = new StackPanel { Spacing = 6 };
        foreach (var element in elements) panel.Children.Add(element);
        return panel;
    }

    private TextBox ContentText(string label, string value)
    {
        var field = new TextBox { Header = label, Text = value, MaxLength = 9 };
        AutomationProperties.SetName(field, label);
        _inspectorInputs.Add(field);
        return field;
    }

    private NumberBox ContentNumber(string label, double minimum, double maximum, double value)
    {
        var field = new NumberBox { Header = label, Minimum = minimum, Maximum = maximum, Value = value,
            SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Inline };
        AutomationProperties.SetName(field, label);
        _inspectorInputs.Add(field);
        return field;
    }

    private void AddContentApply(StackPanel panel, string label, Action apply)
    {
        var button = StudioBrushes.GhostButton(label);
        AutomationProperties.SetName(button, label);
        button.Click += (_, _) => { CommitPendingText(); apply(); };
        _inspectorInputs.Add(button);
        panel.Children.Add(button);
    }

    private async Task PickDesignImageAsync()
    {
        if (!CanEdit || _session is not { } session || session.ImportBusy) return;
        CommitPendingText();
        try
        {
            var picker = new Windows.Storage.Pickers.FileOpenPicker();
            foreach (var extension in new[] { ".png", ".jpg", ".jpeg", ".svg" }) picker.FileTypeFilter.Add(extension);
            WinRT.Interop.InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
            var file = await picker.PickSingleFileAsync();
            if (file is null || !ReferenceEquals(_session, session)) return;
            await session.ImportDesignImageAsync(file.Path);
            Say(session.ImportStatus);
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Say("Could not import the image. " + ex.Message);
        }
    }
}
