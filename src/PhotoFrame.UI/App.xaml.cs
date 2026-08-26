using System.Windows;
using PhotoFrame.Core.Services;

namespace PhotoFrame.UI;

public partial class App : Application
{
    private MainWindow? _mainWindow;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        _mainWindow = new MainWindow();
        _mainWindow.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _mainWindow?.SaveSettings();
        base.OnExit(e);
    }
}
