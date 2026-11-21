#ifndef BIBLIOLED_APPLICATION_PATHS_H
#define BIBLIOLED_APPLICATION_PATHS_H

#include <string>

namespace biblioled {

// Wide strings preserve Windows filenames independently of the active code page.
struct ApplicationPaths {
    std::wstring application_directory;
    std::wstring config_directory;
    std::wstring data_directory;
    std::wstring backup_directory;
    std::wstring export_directory;
    std::wstring log_directory;
    std::wstring settings_file;
    std::wstring database_file;
    std::wstring database_marker_file;
};

// Resolve the portable folder layout from an absolute executable filename.
// This function does not inspect the filesystem or depend on the process CWD.
bool BuildApplicationPaths(const std::wstring& executable_path,
                           ApplicationPaths* paths,
                           std::wstring* error);

} // namespace biblioled

#endif
