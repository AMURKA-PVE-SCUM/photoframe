using System.IO;

namespace PhotoFrame.Core.Services;

public class ImageLoaderService
{
    private static readonly HashSet<string> SupportedExtensions =
        new(StringComparer.OrdinalIgnoreCase)
        {
            ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".tiff", ".tif"
        };

    public List<string> LoadImages(string folderPath, bool includeSubfolders = false)
    {
        if (string.IsNullOrWhiteSpace(folderPath) || !Directory.Exists(folderPath))
            return new List<string>();

        var searchOption = includeSubfolders
            ? SearchOption.AllDirectories
            : SearchOption.TopDirectoryOnly;

        try
        {
            return Directory.EnumerateFiles(folderPath, "*.*", searchOption)
                .Where(f => SupportedExtensions.Contains(Path.GetExtension(f)))
                .OrderBy(_ => Random.Shared.Next())
                .ToList();
        }
        catch
        {
            return new List<string>();
        }
    }

    public FileSystemWatcher? WatchFolder(string folderPath, Action onChange, bool includeSubfolders = false)
    {
        if (string.IsNullOrWhiteSpace(folderPath) || !Directory.Exists(folderPath))
            return null;

        try
        {
            var watcher = new FileSystemWatcher(folderPath)
            {
                NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.CreationTime,
                IncludeSubdirectories = includeSubfolders,
                EnableRaisingEvents = true
            };

            watcher.Created += (_, _) => onChange();
            watcher.Deleted += (_, _) => onChange();
            watcher.Renamed += (_, _) => onChange();

            return watcher;
        }
        catch
        {
            return null;
        }
    }
}
