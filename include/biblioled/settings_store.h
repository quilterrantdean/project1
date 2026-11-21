#ifndef BIBLIOLED_SETTINGS_STORE_H
#define BIBLIOLED_SETTINGS_STORE_H

#include <string>

namespace biblioled {

enum { kSettingsColumnCount = 5 };

struct AppSettings {
    int daily_limit;
    int monthly_limit;
    int monthly_window_days;
    int loan_duration_days;
    bool diagnostics_enabled;
    unsigned long audit_log_max_size_bytes;
    std::string backup_compression_format;
    int backup_compression_level;
    int column_order[kSettingsColumnCount];
    int column_widths[kSettingsColumnCount];

    AppSettings()
        : daily_limit(5), monthly_limit(20),
          monthly_window_days(30), loan_duration_days(21),
          diagnostics_enabled(true),
          audit_log_max_size_bytes(5UL * 1024UL * 1024UL),
          backup_compression_format("gzip"), backup_compression_level(6)
    {
        for (int i = 0; i < kSettingsColumnCount; ++i) column_order[i] = i;
        column_widths[0] = 320;
        column_widths[1] = 100;
        column_widths[2] = 130;
        column_widths[3] = 130;
        column_widths[4] = 55;
    }
};

enum SettingsLoadStatus {
    kSettingsLoadError = 0,
    kSettingsLoadLoaded,
    kSettingsLoadCreatedDefaults,
    kSettingsLoadInvalidFile
};

// File paths are UTF-8; Win32 file access is performed using UTF-16.
// Missing files are created atomically with defaults. Invalid files are preserved.
bool LoadSettingsFile(const std::string& utf8_path,
                      AppSettings* settings,
                      std::string* error,
                      SettingsLoadStatus* status);
bool LoadSettingsFile(const std::string& utf8_path,
                      AppSettings* settings,
                      std::string* error);
bool SaveSettingsFile(const std::string& utf8_path,
                      const AppSettings& settings,
                      std::string* error);

// Archive an invalid settings file as "<path>.invalid" without overwriting an
// existing archive. The original remains in place if the archive operation fails.
bool ArchiveInvalidSettingsFile(const std::string& utf8_path,
                                std::string* archived_utf8_path,
                                std::string* error);

} // namespace biblioled

#endif
