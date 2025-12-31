using System.Globalization;
using System.Windows.Data;

namespace BrirSpatializer.App;

public sealed class BooleanToStartStopConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        return value is bool running && running ? "Stop" : "Start";
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        throw new NotSupportedException();
    }
}

