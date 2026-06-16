using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Data;

namespace CoreVideoPro.WinUI.Converters;

public sealed class StudioTabEqualsConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, string language)
    {
        var activeTab = value is StudioTab tab ? tab : StudioTab.Studio;
        var expected = Enum.TryParse<StudioTab>(parameter?.ToString(), ignoreCase: true, out var parsed)
            ? parsed
            : StudioTab.Studio;

        var matches = activeTab == expected;

        if (targetType == typeof(Visibility))
        {
            return matches ? Visibility.Visible : Visibility.Collapsed;
        }

        return matches;
    }

    public object ConvertBack(object value, Type targetType, object parameter, string language) =>
        throw new NotSupportedException();
}