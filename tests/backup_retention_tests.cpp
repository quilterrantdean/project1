#include "biblioled/backup_retention.h"
#include "biblioled/backup_compression.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

bool MakeDirectory(std::string* directory)
{
#ifdef _WIN32
    char temporary[MAX_PATH] = {0};
    char name[MAX_PATH] = {0};
    if (!GetTempPathA(MAX_PATH, temporary)) return false;
    if (!GetTempFileNameA(temporary, "BLR", 0, name)) return false;
    DeleteFileA(name);
    if (!CreateDirectoryA(name, NULL)) return false;
    *directory = name;
    return true;
#else
    const std::string path = "backup-retention-test-" + std::to_string(std::rand());
    if (mkdir(path.c_str(), 0700) != 0) return false;
    *directory = path;
    return true;
#endif
}

std::string JoinPath(const std::string& directory, const std::string& name)
{
    return directory + "/" + name;
}

bool WriteDummy(const std::string& path)
{
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << "test backup";
    return output.good();
}

void ListNames(const std::string& directory, std::vector<std::string>* names)
{
    names->clear();
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    const std::string pattern = directory + "\\*";
    HANDLE find = FindFirstFileA(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        names->push_back(data.cFileName);
    } while (FindNextFileA(find, &data));
    FindClose(find);
#else
    DIR* dir = opendir(directory.c_str());
    if (!dir) return;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..") names->push_back(name);
    }
    closedir(dir);
#endif
}

void RemoveDirectoryFiles(const std::string& directory)
{
    std::vector<std::string> names;
    ListNames(directory, &names);
    for (size_t i = 0; i < names.size(); ++i) {
        biblioled::RemoveBackupFile(JoinPath(directory, names[i]));
    }
#ifdef _WIN32
    RemoveDirectoryA(directory.c_str());
#else
    rmdir(directory.c_str());
#endif
}

bool DateForAge(int age, std::string* date)
{
    const std::time_t now = std::time(NULL);
    const std::tm* current = std::localtime(&now);
    if (!current) return false;
    std::tm value = *current;
    value.tm_hour = 12;
    value.tm_min = 0;
    value.tm_sec = 0;
    value.tm_mday -= age;
    value.tm_isdst = -1;
    const std::time_t target = std::mktime(&value);
    if (target == static_cast<std::time_t>(-1)) return false;
    const std::tm* local = std::localtime(&target);
    if (!local) return false;
    char buffer[16] = {0};
    if (!std::strftime(buffer, sizeof(buffer), "%Y%m%d", local)) return false;
    *date = buffer;
    return true;
}

} // namespace

int main()
{
    std::string directory;
    Check(MakeDirectory(&directory),
          "isolated temporary backup directory should be created");
    if (directory.empty()) return 1;

    for (int age = 0; age <= 400; ++age) {
        std::string date;
        Check(DateForAge(age, &date), "test date should be generated");
        if (date.empty()) continue;
        const std::string name = "BiblioLED_auto_" + date + ".sqlite.gz";
        Check(WriteDummy(JoinPath(directory, name)),
              "automatic backup placeholder should be created");
    }

    std::string today_date;
    Check(DateForAge(0, &today_date), "today's backup date should be generated");
    const std::string daily_prefix = "BiblioLED_auto_" + today_date;
    const std::string earlier_same_day = daily_prefix + "_20261114_120000.sqlite.gz";
    const std::string later_same_day = daily_prefix + "_20261114_130000.sqlite.gz";
    Check(WriteDummy(JoinPath(directory, earlier_same_day)),
          "first timestamp-suffixed daily backup should be created");
    Check(WriteDummy(JoinPath(directory, later_same_day)),
          "second timestamp-suffixed daily backup should be created");

    Check(WriteDummy(JoinPath(directory, "BiblioLED_auto_20260231.sqlite.gz")),
          "invalid calendar date backup name should be created");
    Check(WriteDummy(JoinPath(directory, daily_prefix + "_garbage.sqlite.gz")),
          "malformed automatic backup suffix should be created");

    Check(WriteDummy(JoinPath(directory, "BiblioLED_manual_20261114_120000.sqlite.gz")),
          "manual backup placeholder should be created");
    Check(WriteDummy(JoinPath(directory, "BiblioLED_safety_restore_20261114_120000.sqlite.gz")),
          "safety backup placeholder should be created");
    Check(WriteDummy(JoinPath(directory, "BiblioLED_auto_unrecognised.sqlite.gz")),
          "unrecognised automatic filename should be created");

    std::string error;
    Check(biblioled::PruneAutomaticBackups(directory, &error),
          "automatic backup retention should complete");
    if (!error.empty()) std::cerr << error << std::endl;

    std::vector<std::string> names;
    ListNames(directory, &names);
    int automatic_count = 0;
    bool manual_kept = false;
    bool safety_kept = false;
    bool unknown_kept = false;
    bool latest_same_day_kept = false;
    bool earlier_same_day_removed = true;
    bool base_same_day_removed = true;
    bool invalid_calendar_date_kept = false;
    bool malformed_suffix_kept = false;
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i].compare(0, 15, "BiblioLED_auto_") == 0) ++automatic_count;
        if (names[i].find("BiblioLED_manual_") == 0) manual_kept = true;
        if (names[i].find("BiblioLED_safety_") == 0) safety_kept = true;
        if (names[i] == "BiblioLED_auto_unrecognised.sqlite.gz") unknown_kept = true;
        if (names[i] == later_same_day) latest_same_day_kept = true;
        if (names[i] == earlier_same_day) earlier_same_day_removed = false;
        if (names[i] == daily_prefix + ".sqlite.gz") base_same_day_removed = false;
        if (names[i] == "BiblioLED_auto_20260231.sqlite.gz") invalid_calendar_date_kept = true;
        if (names[i] == daily_prefix + "_garbage.sqlite.gz") malformed_suffix_kept = true;
    }

    // 7 daily + 4 consecutive weekly periods + 12 calendar-month snapshots,
    // plus three unrecognised/malformed files preserved rather than guessed at.
    Check(automatic_count == 26,
          "retention should keep 7 daily, 4 weekly, 12 monthly and three malformed/unknown files");
    Check(latest_same_day_kept && earlier_same_day_removed && base_same_day_removed,
          "retention should recognise timestamp suffixes and keep only the latest same-day snapshot");
    Check(manual_kept, "manual backups must never be auto-pruned");
    Check(safety_kept, "restore/migration safety backups must never be auto-pruned");
    Check(unknown_kept && invalid_calendar_date_kept && malformed_suffix_kept,
          "unrecognised or malformed automatic backup filenames must be preserved");

    RemoveDirectoryFiles(directory);
    if (failures != 0) {
        std::cerr << failures << " backup-retention assertion(s) failed." << std::endl;
        return 1;
    }
    std::cout << "Backup retention tests passed." << std::endl;
    return 0;
}
