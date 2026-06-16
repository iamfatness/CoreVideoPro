using CoreVideoPro.WinUI.Models;
using Microsoft.UI;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Converters;

public sealed class TabActiveConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, string language)
    {
        var parts = parameter?.ToString()?.Split('|', 2) ?? [];
        if (parts.Length != 2)
        {
            return new SolidColorBrush(Colors.Transparent);
        }

        var tab = parts[0];
        var property = parts[1];
        var activeTab = value is StudioTab studioTab ? studioTab.ToString() : value?.ToString();
        var active = string.Equals(activeTab, tab, StringComparison.OrdinalIgnoreCase);

        var color = property switch
        {
            "Background" => active ? ColorHelper.FromArgb(255, 26, 61, 46) : ColorHelper.FromArgb(0, 0, 0, 0),
            "BorderBrush" => active ? ColorHelper.FromArgb(255, 61, 220, 151) : ColorHelper.FromArgb(255, 42, 52, 60),
            "Foreground" => active ? ColorHelper.FromArgb(255, 232, 240, 236) : ColorHelper.FromArgb(255, 148, 165, 155),
            _ => ColorHelper.FromArgb(0, 0, 0, 0)
        };

        return new SolidColorBrush(color);
    }

    public object ConvertBack(object value, Type targetType, object parameter, string language) =>
        throw new NotSupportedException();
}