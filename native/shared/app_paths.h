#ifndef CHROMEYUMM_APP_PATHS_H
#define CHROMEYUMM_APP_PATHS_H

#include <string>

namespace chromeyumm {

/**
 * Build the app data path using identifier/channel structure.
 * This ensures consistent path structure across all platforms and matches
 * the CLI and updater conventions.
 *
 * @param basePath The base application support/data path (e.g., ~/Library/Application Support)
 * @param identifier The app identifier (e.g., "chromeyumm")
 * @param channel The release channel (e.g., "dev", "canary", "stable")
 * @param suffix Optional suffix to append (e.g., "CEF")
 * @param pathSeparator The path separator to use ('/' for Unix, '\\' for Windows)
 * @return The full path: basePath/identifier/channel/suffix
 */
inline std::string buildAppDataPath(
    const std::string& basePath,
    const std::string& identifier,
    const std::string& channel,
    const std::string& suffix = "",
    char pathSeparator = '/'
) {
    std::string appId = !identifier.empty() ? identifier : "Chromeyumm";
    std::string channelPath = !channel.empty() ? channel : "default";

    std::string result = basePath;
    result += pathSeparator;
    result += appId;
    result += pathSeparator;
    result += channelPath;

    if (!suffix.empty()) {
        result += pathSeparator;
        result += suffix;
    }

    return result;
}

/**
 * Build a partition-specific path under the app data directory.
 *
 * @param basePath The base application support/data path
 * @param identifier The app identifier
 * @param channel The release channel
 * @param renderer The renderer type (e.g., "CEF")
 * @param partitionName The partition name
 * @param pathSeparator The path separator to use
 * @return The full path: basePath/identifier/channel/renderer/Partitions/partitionName
 */
inline std::string buildPartitionPath(
    const std::string& basePath,
    const std::string& identifier,
    const std::string& channel,
    const std::string& renderer,
    const std::string& partitionName,
    char pathSeparator = '/'
) {
    std::string base = buildAppDataPath(basePath, identifier, channel, renderer, pathSeparator);
    base += pathSeparator;
    base += "Partitions";
    base += pathSeparator;
    base += partitionName;
    return base;
}

} // namespace chromeyumm

#endif // CHROMEYUMM_APP_PATHS_H
