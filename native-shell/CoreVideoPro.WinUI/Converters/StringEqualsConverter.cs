using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Data;

namespace CoreVideoPro.WinUI.Converters;

public sealed class StringEqualsConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, string language)
    {
        var match = value?.ToString() == parameter?.ToString();
        if (targetType == typeof(Visibility))
        {
            return match ? Visibility.Visible : Visibility.Collapsed;
        }

        return match;
    }

    public object ConvertBack(object value, Type targetType, object parameter, string language) =>
        throw new NotSupportedException();
}