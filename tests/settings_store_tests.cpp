#include "biblioled/settings_store.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {
int failures = 0;
std::string test_parent_directory;

void Check(bool condition, const char* message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

#ifdef _WIN32
std::string WideToUtf8(const std::wstring& value)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
                                           NULL, 0, NULL, NULL);
    if (length <= 1) return std::string();
    std::string result(static_cast<size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
                             &result[0], length, NULL, NULL)) return std::string();
    result.resize(static_cast<size_t>(length - 1));
    return result;
}
std::wstring Utf8ToWide(const std::string& value)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, NULL, 0);
    if (length <= 1) return std::wstring();
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1,
                             &result[0], length)) return std::wstring();
    result.resize(static_cast<size_t>(length - 1));
    return result;
}
bool MakeTestDirectory(std::string* directory)
{
    if (!directory) return false;
    wchar_t temporary[MAX_PATH] = {0};
    const DWORD length = GetTempPathW(MAX_PATH, temporary);
    if (length == 0 || length >= MAX_PATH) return false;
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        std::wostringstream name;
        name << std::wstring(temporary, length)
             << L"BiblioLED_Settings_" << GetCurrentProcessId()
             << L"_" << attempt << L"_ação";
        if (CreateDirectoryW(name.str().c_str(), NULL)) {
            *directory = WideToUtf8(name.str());
            return !directory->empty();
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}
long CurrentProcessId() { return static_cast<long>(GetCurrentProcessId()); }
bool RemoveFile(const std::string& p)
{
    if (DeleteFileW(Utf8ToWide(p).c_str())) return true;
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}
bool RemoveDir(const std::string& p) { return RemoveDirectoryW(Utf8ToWide(p).c_str()) != 0; }
bool MakeDirectory(const std::string& p) { return CreateDirectoryW(Utf8ToWide(p).c_str(), NULL) != 0; }
bool WriteRawFile(const std::string& p, const std::string& data)
{
    HANDLE file = CreateFileW(Utf8ToWide(p).c_str(), GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t offset = 0; bool ok = true;
    while (offset < data.size()) {
        const DWORD request = static_cast<DWORD>(
            data.size() - offset > 65536U ? 65536U : data.size() - offset);
        DWORD written = 0;
        if (!WriteFile(file, data.data() + offset, request, &written, NULL) || !written) {
            ok = false; break;
        }
        offset += written;
    }
    if (!CloseHandle(file)) ok = false;
    return ok;
}
bool ReadRawFile(const std::string& p, std::string* data)
{
    if (!data) return false;
    HANDLE file = CreateFileW(Utf8ToWide(p).c_str(), GENERIC_READ, FILE_SHARE_READ,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    data->clear(); char buffer[4096]; bool ok = true;
    for (;;) {
        DWORD received = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &received, NULL)) { ok = false; break; }
        if (!received) break;
        data->append(buffer, received);
    }
    if (!CloseHandle(file)) ok = false;
    return ok;
}
bool FileExists(const std::string& p)
{
    return GetFileAttributesW(Utf8ToWide(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}
#else
bool MakeTestDirectory(std::string* directory)
{
    if (!directory) return false;
    char pattern[] = "/tmp/BiblioLED_Settings_XXXXXX";
    char* base = mkdtemp(pattern);
    if (!base) return false;
    test_parent_directory = base;
    std::string candidate(base);
    candidate += "/ação";
    if (mkdir(candidate.c_str(), 0700) != 0) return false;
    *directory = candidate;
    return true;
}
long CurrentProcessId() { return static_cast<long>(getpid()); }
bool RemoveFile(const std::string& p) { return unlink(p.c_str()) == 0; }
bool RemoveDir(const std::string& p) { return rmdir(p.c_str()) == 0; }
bool MakeDirectory(const std::string& p) { return mkdir(p.c_str(), 0700) == 0; }
bool WriteRawFile(const std::string& p, const std::string& data)
{
    std::ofstream out(p.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush(); const bool ok = out.good(); out.close();
    return ok && !out.fail();
}
bool ReadRawFile(const std::string& p, std::string* data)
{
    if (!data) return false;
    std::ifstream in(p.c_str(), std::ios::binary);
    if (!in) return false;
    std::ostringstream out; out << in.rdbuf();
    if (in.bad()) return false;
    *data = out.str(); return true;
}
bool FileExists(const std::string& p)
{
    struct stat s; return stat(p.c_str(), &s) == 0;
}
#endif

std::string Join(const std::string& d, const std::string& n)
{
#ifdef _WIN32
    return d + "\\" + n;
#else
    return d + "/" + n;
#endif
}

void CheckDefaults(const biblioled::AppSettings& s)
{
    Check(s.daily_limit == 5 && s.monthly_limit == 20 &&
          s.monthly_window_days == 30 && s.loan_duration_days == 21,
          "default limits should match the application contract");
    Check(s.diagnostics_enabled &&
          s.audit_log_max_size_bytes == 5UL * 1024UL * 1024UL,
          "default diagnostic settings should be enabled with a 5 MiB limit");
    Check(s.backup_compression_format == "gzip" &&
          s.backup_compression_level == 6,
          "default backup compression should be gzip level six");
    const int widths[5] = {320, 100, 130, 130, 55};
    for (int i = 0; i < 5; ++i) {
        Check(s.column_order[i] == i && s.column_widths[i] == widths[i],
              "default column layout should match the application");
    }
}

void TestMissingFileCreatesDefaults(const std::string& dir)
{
    const std::string path = Join(dir, "missing.ini");
    biblioled::AppSettings settings; settings.daily_limit = 99;
    std::string error;
    biblioled::SettingsLoadStatus status = biblioled::kSettingsLoadError;
    Check(biblioled::LoadSettingsFile(path, &settings, &error, &status),
          "a missing INI should be created atomically with defaults");
    Check(status == biblioled::kSettingsLoadCreatedDefaults,
          "missing-file creation should report the defaults-created load status");
    CheckDefaults(settings);
    std::string bytes;
    Check(ReadRawFile(path, &bytes) &&
          bytes.find("daily_limit=5\r\n") != std::string::npos,
          "the new defaults file should contain the standard CRLF form");
    biblioled::AppSettings reopened;
    Check(biblioled::LoadSettingsFile(path, &reopened, &error, &status),
          "the newly created defaults file should load normally");
    Check(status == biblioled::kSettingsLoadLoaded,
          "a valid existing INI should report the loaded status");
    CheckDefaults(reopened);
}

void TestRoundTrip(const std::string& dir)
{
    const std::string path = Join(dir, "roundtrip.ini");
    biblioled::AppSettings expected;
    expected.daily_limit = 12; expected.monthly_limit = 45;
    expected.monthly_window_days = 60; expected.loan_duration_days = 28;
    expected.diagnostics_enabled = false;
    expected.audit_log_max_size_bytes = 8UL * 1024UL * 1024UL;
    expected.backup_compression_format = "zlib";
    expected.backup_compression_level = 0;
    const int order[5] = {2, 0, 4, 1, 3};
    const int widths[5] = {410, 90, 140, 150, 65};
    for (int i = 0; i < 5; ++i) {
        expected.column_order[i] = order[i];
        expected.column_widths[i] = widths[i];
    }
    std::string error;
    Check(biblioled::SaveSettingsFile(path, expected, &error),
          "valid settings should be saved with atomic replacement");

    // A second save exercises the replace-existing path used by later UI edits.
    expected.daily_limit = 13;
    expected.backup_compression_level = 8;
    Check(biblioled::SaveSettingsFile(path, expected, &error),
          "a second valid save should atomically replace the existing INI");
    biblioled::AppSettings actual;
    Check(biblioled::LoadSettingsFile(path, &actual, &error),
          "saved settings should parse on reload");
    Check(actual.daily_limit == expected.daily_limit &&
          actual.monthly_limit == expected.monthly_limit &&
          actual.monthly_window_days == expected.monthly_window_days &&
          actual.loan_duration_days == expected.loan_duration_days,
          "general settings should survive save/load");
    Check(actual.diagnostics_enabled == expected.diagnostics_enabled &&
          actual.audit_log_max_size_bytes == expected.audit_log_max_size_bytes,
          "diagnostic settings should survive save/load");
    Check(actual.backup_compression_format == expected.backup_compression_format &&
          actual.backup_compression_level == expected.backup_compression_level,
          "backup settings should survive save/load");
    for (int i = 0; i < 5; ++i) {
        Check(actual.column_order[i] == expected.column_order[i] &&
              actual.column_widths[i] == expected.column_widths[i],
              "column layout should survive save/load");
    }
}

void TestInvalidFileIsPreserved(const std::string& dir)
{
    const std::string path = Join(dir, "invalid.ini");
    const std::string original =
        "; preserve this invalid file\r\n[limits]\r\n"
        "daily_limit=0\r\nmonthly_limit=20\r\n"
        "monthly_window_days=30\r\nloan_duration_days=21\r\n";
    Check(WriteRawFile(path, original), "invalid INI fixture should be written");
    biblioled::AppSettings settings; settings.daily_limit = 999;
    std::string error;
    biblioled::SettingsLoadStatus status = biblioled::kSettingsLoadError;
    Check(!biblioled::LoadSettingsFile(path, &settings, &error, &status),
          "an out-of-range setting should be rejected");
    Check(status == biblioled::kSettingsLoadInvalidFile,
          "malformed settings content should report the invalid-file status");
    CheckDefaults(settings);
    std::string after;
    Check(ReadRawFile(path, &after) && after == original,
          "loading an invalid INI must preserve the original bytes");

    biblioled::AppSettings valid_update;
    valid_update.daily_limit = 12;
    error.clear();
    Check(!biblioled::SaveSettingsFile(path, valid_update, &error),
          "the storage service must refuse to save over an existing invalid INI");
    Check(!error.empty(), "refusing to replace an invalid INI should explain why");
    Check(ReadRawFile(path, &after) && after == original,
          "even a valid in-memory update must not replace an invalid existing INI");
}

void TestDuplicateKeyAndBom(const std::string& dir)
{
    const std::string path = Join(dir, "duplicate.ini");
    const std::string original =
        "\xEF\xBB\xBF; UTF-8 BOM\r\n[limits]\r\n"
        "daily_limit=5\r\ndaily_limit=6\r\nmonthly_limit=20\r\n"
        "monthly_window_days=30\r\nloan_duration_days=21\r\n";
    Check(WriteRawFile(path, original), "duplicate-key INI should be written");
    biblioled::AppSettings settings; std::string error;
    Check(!biblioled::LoadSettingsFile(path, &settings, &error),
          "duplicate known keys should be rejected");
    std::string after;
    Check(ReadRawFile(path, &after) && after == original,
          "duplicate-key INI should remain unchanged");
}


void TestArchiveInvalidSettingsFile(const std::string& dir)
{
    const std::string path = Join(dir, "archive.ini");
    const std::string archived = path + ".invalid";
    const std::string original =
        "; preserve and archive this invalid file\r\n[limits]\r\n"
        "daily_limit=0\r\nmonthly_limit=20\r\n"
        "monthly_window_days=30\r\nloan_duration_days=21\r\n";

    Check(WriteRawFile(path, original),
          "invalid settings file for archive test should be written");
    std::string archived_path;
    std::string error;
    Check(biblioled::ArchiveInvalidSettingsFile(path, &archived_path, &error),
          "user-confirmed archive of an invalid settings file should succeed");
    Check(archived_path == archived,
          "archive operation should report the exact .invalid path");
    Check(!FileExists(path) && FileExists(archived),
          "successful archive should move the invalid source to the .invalid name");

    std::string archived_bytes;
    Check(ReadRawFile(archived, &archived_bytes) && archived_bytes == original,
          "archived invalid settings must remain byte-for-byte identical");

    biblioled::AppSettings defaults;
    biblioled::SettingsLoadStatus status = biblioled::kSettingsLoadError;
    error.clear();
    Check(biblioled::LoadSettingsFile(path, &defaults, &error, &status),
          "loading after archiving should create a fresh defaults INI");
    Check(status == biblioled::kSettingsLoadCreatedDefaults,
          "recreated settings should report defaults-created status");
    CheckDefaults(defaults);
    Check(FileExists(path),
          "a fresh config/settings.ini should exist after recreation");
    Check(ReadRawFile(archived, &archived_bytes) && archived_bytes == original,
          "creating defaults must not change the archived invalid file");

    RemoveFile(path);
    RemoveFile(archived);
}

void TestArchiveCollisionPreservesBothFiles(const std::string& dir)
{
    const std::string path = Join(dir, "archive-collision.ini");
    const std::string archived = path + ".invalid";
    const std::string original = "[limits]\r\ndaily_limit=0\r\n";
    const std::string existing_archive = "previous invalid settings archive";
    Check(WriteRawFile(path, original),
          "invalid source for archive-collision test should be written");
    Check(WriteRawFile(archived, existing_archive),
          "existing .invalid archive sentinel should be written");

    std::string archived_path;
    std::string error;
    Check(!biblioled::ArchiveInvalidSettingsFile(path, &archived_path, &error),
          "archive collision must be rejected instead of overwriting an older .invalid file");
    Check(!error.empty(), "archive collision should explain the failure");
    std::string bytes;
    Check(ReadRawFile(path, &bytes) && bytes == original,
          "archive collision must preserve the current invalid source");
    Check(ReadRawFile(archived, &bytes) && bytes == existing_archive,
          "archive collision must preserve the earlier .invalid archive");

    RemoveFile(path);
    RemoveFile(archived);
}

void TestArchiveRefusesValidSettings(const std::string& dir)
{
    const std::string path = Join(dir, "valid-archive.ini");
    biblioled::AppSettings defaults;
    std::string error;
    Check(biblioled::SaveSettingsFile(path, defaults, &error),
          "valid source for archive-refusal test should be created");

    std::string before;
    Check(ReadRawFile(path, &before),
          "valid settings bytes should be readable before archive attempt");
    std::string archived_path;
    error.clear();
    Check(!biblioled::ArchiveInvalidSettingsFile(path, &archived_path, &error),
          "valid settings must not be archived by the invalid-file operation");
    Check(!error.empty(), "refusing a valid settings file should explain why");

    std::string after;
    Check(ReadRawFile(path, &after) && after == before,
          "refusing to archive valid settings must preserve the file unchanged");
    Check(!FileExists(path + ".invalid"),
          "refusing valid settings must not create an .invalid archive");

    RemoveFile(path);
}

void TestOversizedSettingsCanBeArchived(const std::string& dir)
{
    const std::string path = Join(dir, "oversized-archive.ini");
    const std::string archived = path + ".invalid";
    const std::string original(64U * 1024U + 1U, 'X');
    Check(WriteRawFile(path, original),
          "oversized settings source for archive test should be written");

    std::string archived_path;
    std::string error;
    Check(biblioled::ArchiveInvalidSettingsFile(path, &archived_path, &error),
          "oversized settings files should be eligible for explicit archival");
    Check(!FileExists(path) && FileExists(archived),
          "oversized settings source should move to its .invalid suffix");
    std::string after;
    Check(ReadRawFile(archived, &after) && after == original,
          "oversized settings archive must preserve all original bytes");

    RemoveFile(archived);
}

void TestInvalidSavePreservesOriginal(const std::string& dir)
{
    const std::string path = Join(dir, "invalid-save.ini");
    const std::string original = "[limits]\r\ndaily_limit=5\r\n";
    Check(WriteRawFile(path, original), "original target should be written");
    biblioled::AppSettings invalid; invalid.daily_limit = 0;
    std::string error;
    Check(!biblioled::SaveSettingsFile(path, invalid, &error),
          "invalid in-memory settings must be rejected");
    std::string after;
    Check(ReadRawFile(path, &after) && after == original,
          "invalid in-memory settings must not replace the previous file");
}

void TestTemporaryCollision(const std::string& dir)
{
    const std::string path = Join(dir, "collision.ini");
    std::ostringstream name;
    name << path << ".tmp." << CurrentProcessId() << ".0";
    const std::string stale = name.str();
    const std::string sentinel = "do not overwrite this file";
    Check(WriteRawFile(stale, sentinel), "stale temp-name fixture should be written");
    biblioled::AppSettings settings; settings.daily_limit = 17;
    std::string error;
    Check(biblioled::SaveSettingsFile(path, settings, &error),
          "writer should skip a stale temporary filename");
    std::string after;
    Check(ReadRawFile(stale, &after) && after == sentinel,
          "writer must never overwrite an existing temporary file");
    biblioled::AppSettings loaded;
    Check(biblioled::LoadSettingsFile(path, &loaded, &error) &&
          loaded.daily_limit == 17,
          "settings saved after a temp collision should reload");
    RemoveFile(stale);
}

void TestReplaceFailurePreservesTarget(const std::string& dir)
{
    const std::string target = Join(dir, "blocked.ini");
    Check(MakeDirectory(target), "replacement-failure target directory should be created");
    const std::string sentinel_path = Join(target, "keep.txt");
    const std::string sentinel = "existing target must remain intact";
    Check(WriteRawFile(sentinel_path, sentinel), "replacement sentinel should be written");
    biblioled::AppSettings settings; settings.daily_limit = 23;
    std::string error;
    Check(!biblioled::SaveSettingsFile(target, settings, &error),
          "atomic replacement of a non-empty directory should fail");
    std::string after;
    Check(ReadRawFile(sentinel_path, &after) && after == sentinel,
          "failed replacement must leave existing target contents intact");
    std::ostringstream temp;
    temp << target << ".tmp." << CurrentProcessId() << ".0";
    Check(!FileExists(temp.str()), "failed replacement must remove its temporary file");
    RemoveFile(sentinel_path); RemoveDir(target);
}

void TestDirectoryIsNotMissingFile(const std::string& dir)
{
    biblioled::AppSettings settings; std::string error;
    Check(!biblioled::LoadSettingsFile(dir, &settings, &error),
          "a directory at the INI path must be a read error, not a missing file");
    Check(!error.empty(), "directory/read errors should be reported");
}

void TestOversizedFilePreserved(const std::string& dir)
{
    const std::string path = Join(dir, "oversized.ini");
    const std::string original(64U * 1024U + 1U, 'X');
    Check(WriteRawFile(path, original), "oversized INI fixture should be written");
    biblioled::AppSettings settings; std::string error;
    biblioled::SettingsLoadStatus status = biblioled::kSettingsLoadError;
    Check(!biblioled::LoadSettingsFile(path, &settings, &error, &status),
          "settings files larger than 64 KiB should be rejected");
    Check(status == biblioled::kSettingsLoadInvalidFile,
          "an oversized settings file should be classified as invalid content");
    CheckDefaults(settings);
    std::string after;
    Check(ReadRawFile(path, &after) && after == original,
          "oversized settings should remain unchanged");
}

} // namespace

int main()
{
    std::string directory;
    Check(MakeTestDirectory(&directory),
          "isolated Unicode-named settings directory should be created");
    if (directory.empty()) return EXIT_FAILURE;

    TestMissingFileCreatesDefaults(directory);
    TestRoundTrip(directory);
    TestInvalidFileIsPreserved(directory);
    TestArchiveInvalidSettingsFile(directory);
    TestArchiveCollisionPreservesBothFiles(directory);
    TestArchiveRefusesValidSettings(directory);
    TestOversizedSettingsCanBeArchived(directory);
    TestDuplicateKeyAndBom(directory);
    TestInvalidSavePreservesOriginal(directory);
    TestTemporaryCollision(directory);
    TestReplaceFailurePreservesTarget(directory);
    TestDirectoryIsNotMissingFile(directory);
    TestOversizedFilePreserved(directory);

    const char* names[] = {
        "missing.ini", "roundtrip.ini", "invalid.ini", "invalid.ini.invalid",
        "archive.ini", "archive.ini.invalid", "archive-collision.ini",
        "archive-collision.ini.invalid", "valid-archive.ini",
        "oversized-archive.ini.invalid", "duplicate.ini",
        "invalid-save.ini", "collision.ini", "oversized.ini"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        RemoveFile(Join(directory, names[i]));
    }
    RemoveDir(directory);
#ifndef _WIN32
    if (!test_parent_directory.empty()) rmdir(test_parent_directory.c_str());
#endif

    if (failures != 0) {
        std::cerr << failures << " settings-store test(s) failed." << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "All settings-store tests passed." << std::endl;
    return EXIT_SUCCESS;
}
