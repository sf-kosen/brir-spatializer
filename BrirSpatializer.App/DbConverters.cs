using System.Globalization;
using System.Windows.Data;

namespace BrirSpatializer.App;

public sealed class DbToNeedleAngleConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is not float db) return 0.0;
        float clamped = Math.Clamp(db, -20.0f, 3.0f);
        float t = (clamped + 20.0f) / 23.0f;
        return -40.0 + (60.0 * t);
    }

    public object ConvertBack(object value, Type targetType, object? parameter, CultureInfo culture) => Binding.DoNothing;
}

public sealed class DbToBarHeightConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is not float db) return 0.0;
        double maxHeight = 120.0;
        if (parameter is string param && double.TryParse(param, out double parsedHeight))
        {
            maxHeight = parsedHeight;
        }

        float clamped = Math.Clamp(db, -60.0f, 0.0f);
        float t = (clamped + 60.0f) / 60.0f;
        return maxHeight * t;
    }

    public object ConvertBack(object value, Type targetType, object? parameter, CultureInfo culture) => Binding.DoNothing;
}

