#include "biblioled/backup_retention.h"
#include "biblioled/backup_compression.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace biblioled {
namespace {

void SetError(std::string* error, const std::string& message)
{
    if (error) *error = message;
}

std::string JoinPath(const std::string& directory, const std::string& name)
{
    if (directory.empty()) return name;
    const char last = directory[directory.size() - 1];
    if (last == '/' || last == '\\') return directory + name;
#ifdef _WIN32
    return directory + "\\" + name;
#else
    return directory + "/" + name;
#endif
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& value)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), NULL, 0);
    if (length <= 0) return std::wstring();
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
            static_cast<int>(value.size()), &result[0], length)) return std::wstring();
    return result;
}

std::string WideToUtf8(const std::wstring& value)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), NULL, 0, NULL, NULL);
    if (length <= 0) return std::string();
    std::string result(static_cast<size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
            static_cast<int>(value.size()), &result[0], length, NULL, NULL)) return std::string();
    return result;
}
#endif

bool ListAutomaticBackups(const std::string& directory,
                          std::vector<std::string>* names,
                          std::string* error)
{
    names->clear();
#ifdef _WIN32
    std::wstring wide_directory = Utf8ToWide(directory);
    if (wide_directory.empty()) {
        SetError(error, "Could not convert the backup directory path.");
        return false;
    }
    if (!wide_directory.empty() &&
        wide_directory[wide_directory.size() - 1] != L'\\' &&
        wide_directory[wide_directory.size() - 1] != L'/') {
        wide_directory += L'\\';
    }
    const std::wstring pattern = wide_directory + L"BiblioLED_auto_*";
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        SetError(error, "Could not enumerate automatic backup files.");
        return false;
    }
    bool ok = true;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string name = WideToUtf8(data.cFileName);
        if (name.empty()) { ok = false; break; }
        names->push_back(name);
    } while (FindNextFileW(find, &data));
    const DWORD last = GetLastError();
    FindClose(find);
    if (!ok || last != ERROR_NO_MORE_FILES) {
        SetError(error, "Automatic backup enumeration was interrupted.");
        return false;
    }
#else
    DIR* dir = opendir(directory.c_str());
    if (!dir) {
        SetError(error, "Could not open the backup directory for retention.");
        return false;
    }
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        const std::string name(entry->d_name);
        if (name.compare(0, 15, "BiblioLED_auto_") == 0) names->push_back(name);
    }
    closedir(dir);
#endif
    return true;
}

// Gregorian civil date to days since 1970-01-01.
long long DaysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year =
        (153 * adjusted_month + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<long long>(era) * 146097 +
           static_cast<long long>(day_of_era) - 719468;
}

bool IsAsciiDigits(const std::string& value, size_t start, size_t count)
{
    if (start > value.size() || count > value.size() - start) return false;
    for (size_t i = start; i < start + count; ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}

bool ParseBackupDate(const std::string& name, int* year, int* month, int* day)
{
    const std::string prefix = "BiblioLED_auto_";
    if (name.compare(0, prefix.size(), prefix) != 0 ||
        name.size() < prefix.size() + 8 + 7) return false;

    const char* extensions[] = {".sqlite.gz", ".sqlite.zlib", ".sqlite"};
    size_t extension_size = 0;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        const std::string extension(extensions[i]);
        if (name.size() >= extension.size() &&
            name.compare(name.size() - extension.size(), extension.size(), extension) == 0) {
            extension_size = extension.size();
            break;
        }
    }
    if (!extension_size) return false;

    const size_t stem_end = name.size() - extension_size;
    const size_t date_start = prefix.size();
    if (!IsAsciiDigits(name, date_start, 8)) return false;

    // The only optional suffix produced by MakeBackupPath is
    // _YYYYMMDD_HHMMSS[_N]. Do not prune similarly prefixed unknown files.
    const size_t suffix_start = date_start + 8;
    const size_t suffix_size = stem_end - suffix_start;
    if (suffix_size != 0) {
        if (suffix_size < 16 || name[suffix_start] != '_' ||
            !IsAsciiDigits(name, suffix_start + 1, 8) ||
            name[suffix_start + 9] != '_' ||
            !IsAsciiDigits(name, suffix_start + 10, 6)) return false;

        const size_t numeric_suffix = suffix_start + 16;
        if (numeric_suffix < stem_end) {
            const size_t digits = stem_end - numeric_suffix - 1;
            if (name[numeric_suffix] != '_' || digits == 0 || digits > 3 ||
                !IsAsciiDigits(name, numeric_suffix + 1, digits)) return false;
        }
    }

    *year = (name[date_start] - '0') * 1000 + (name[date_start + 1] - '0') * 100 +
            (name[date_start + 2] - '0') * 10 + name[date_start + 3] - '0';
    *month = (name[date_start + 4] - '0') * 10 + name[date_start + 5] - '0';
    *day = (name[date_start + 6] - '0') * 10 + name[date_start + 7] - '0';

    if (*year < 1970 || *month < 1 || *month > 12 || *day < 1) return false;
    static const int days_in_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    int maximum_day = days_in_month[*month - 1];
    const bool leap_year = ((*year % 4 == 0) && (*year % 100 != 0)) ||
                           (*year % 400 == 0);
    if (*month == 2 && leap_year) maximum_day = 29;
    return *day <= maximum_day;
}

long long CurrentDayOrdinal()
{
    const std::time_t now = std::time(NULL);
    const std::tm* local = std::localtime(&now);
    if (!local) return -1;
    return DaysFromCivil(local->tm_year + 1900,
                         static_cast<unsigned>(local->tm_mon + 1),
                         static_cast<unsigned>(local->tm_mday));
}

} // namespace

bool PruneAutomaticBackups(const std::string& directory,
                           std::string* error)
{
    std::vector<std::string> names;
    if (!ListAutomaticBackups(directory, &names, error)) return false;

    const long long today = CurrentDayOrdinal();
    if (today < 0) {
        SetError(error, "Could not determine the local date for backup retention.");
        return false;
    }

    std::set<std::string> keep;
    std::map<long long, std::string> daily;
    std::map<int, std::string> weekly;
    std::map<int, std::string> monthly;
    const std::time_t now = std::time(NULL);
    const std::tm* local = std::localtime(&now);
    if (!local) {
        SetError(error, "Could not determine the local month for backup retention.");
        return false;
    }
    const int current_month = (local->tm_year + 1900) * 12 + local->tm_mon;

    for (size_t i = 0; i < names.size(); ++i) {
        int year = 0, month = 0, day = 0;
        if (!ParseBackupDate(names[i], &year, &month, &day)) {
            // Unknown or malformed files are preserved, never guessed at.
            keep.insert(names[i]);
            continue;
        }
        const long long date = DaysFromCivil(year, static_cast<unsigned>(month),
                                             static_cast<unsigned>(day));
        const long long age = today - date;
        if (age < 0) {
            keep.insert(names[i]);
        } else if (age < 7) {
            std::map<long long, std::string>::iterator found = daily.find(date);
            if (found == daily.end() || found->second < names[i]) daily[date] = names[i];
        } else if (age < 35) {
            const int period = static_cast<int>((age - 7) / 7);
            std::map<int, std::string>::iterator found = weekly.find(period);
            if (found == weekly.end() || found->second < names[i]) weekly[period] = names[i];
        } else {
            const int backup_month = year * 12 + (month - 1);
            const int month_age = current_month - backup_month;
            if (month_age >= 1 && month_age <= 12) {
                std::map<int, std::string>::iterator found = monthly.find(month_age);
                if (found == monthly.end() || found->second < names[i]) monthly[month_age] = names[i];
            }
        }
    }

    for (std::map<long long, std::string>::const_iterator i = daily.begin(); i != daily.end(); ++i) keep.insert(i->second);
    for (std::map<int, std::string>::const_iterator i = weekly.begin(); i != weekly.end(); ++i) keep.insert(i->second);
    for (std::map<int, std::string>::const_iterator i = monthly.begin(); i != monthly.end(); ++i) keep.insert(i->second);

    bool all_removed = true;
    for (size_t i = 0; i < names.size(); ++i) {
        if (keep.find(names[i]) != keep.end()) continue;
        if (!RemoveBackupFile(JoinPath(directory, names[i]))) all_removed = false;
    }
    if (!all_removed) {
        SetError(error, "One or more expired automatic backups could not be removed; the valid new backup remains available.");
        return false;
    }
    return true;
}

} // namespace biblioled
