#include "biblioled/application_paths.h"

#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

void TestDrivePath()
{
    biblioled::ApplicationPaths paths;
    std::wstring error = L"previous error";

    Check(biblioled::BuildApplicationPaths(
              L"C:\\Program Files\\BiblioLED\\BiblioLED.exe",
              &paths, &error),
          "an absolute drive path should be accepted");
    Check(error.empty(), "successful resolution should clear an old error");
    Check(paths.application_directory == L"C:\\Program Files\\BiblioLED",
          "the executable's containing directory should be the application root");
    Check(paths.settings_file ==
              L"C:\\Program Files\\BiblioLED\\config\\settings.ini",
          "the settings path should be rooted beside the executable");
    Check(paths.database_file ==
              L"C:\\Program Files\\BiblioLED\\data\\biblioteca.sqlite",
          "the database path should be rooted beside the executable");
    Check(paths.backup_directory ==
              L"C:\\Program Files\\BiblioLED\\data\\backups",
          "the backup directory should be below the data directory");
    Check(paths.export_directory ==
              L"C:\\Program Files\\BiblioLED\\exports",
          "the export directory should be rooted beside the executable");
    Check(paths.log_directory ==
              L"C:\\Program Files\\BiblioLED\\logs",
          "the log directory should be rooted beside the executable");
}

void TestDriveRoot()
{
    biblioled::ApplicationPaths paths;
    std::wstring error;

    Check(biblioled::BuildApplicationPaths(
              L"C:\\BiblioLED.exe", &paths, &error),
          "an executable directly under a drive root should be accepted");
    Check(paths.application_directory == L"C:\\",
          "a drive-root installation should preserve the root separator");
    Check(paths.config_directory == L"C:\\config",
          "joining a child to a drive root should not create a malformed path");
}

void TestUncPath()
{
    biblioled::ApplicationPaths paths;
    std::wstring error;

    Check(biblioled::BuildApplicationPaths(
              L"\\\\servidor\\partilha\\BiblioLED\\BiblioLED.exe",
              &paths, &error),
          "an absolute UNC executable path should be accepted");
    Check(paths.application_directory ==
              L"\\\\servidor\\partilha\\BiblioLED",
          "a UNC application root should retain the server and share");
    Check(paths.database_file ==
              L"\\\\servidor\\partilha\\BiblioLED\\data\\biblioteca.sqlite",
          "database paths should also work beneath a UNC application root");
}

void TestUnicodeAndForwardSlashes()
{
    biblioled::ApplicationPaths paths;
    std::wstring error;

    Check(biblioled::BuildApplicationPaths(
              L"C:/Utilizadores/Joao/Instala\u00E7\u00E3o/BiblioLED.exe",
              &paths, &error),
          "paths with Unicode characters and forward slashes should be accepted");
    Check(paths.application_directory ==
              L"C:\\Utilizadores\\Joao\\Instala\u00E7\u00E3o",
          "the root should preserve Unicode and normalise separators");
    Check(paths.database_marker_file ==
              L"C:\\Utilizadores\\Joao\\Instala\u00E7\u00E3o\\config\\database.initialized",
          "the marker path should use the resolved Unicode root");
}

void TestInvalidPaths()
{
    biblioled::ApplicationPaths paths;
    std::wstring error;

    Check(!biblioled::BuildApplicationPaths(
              L"BiblioLED.exe", &paths, &error),
          "a bare executable filename must not resolve against the CWD");
    Check(!error.empty(), "a rejected relative path should explain the error");

    Check(biblioled::BuildApplicationPaths(
              L"C:\\Valid\\BiblioLED.exe", &paths, &error),
          "a valid path should populate the output before the failure test");
    Check(!biblioled::BuildApplicationPaths(
              L".\\BiblioLED.exe", &paths, &error),
          "a relative path must be rejected");
    Check(paths.application_directory.empty() &&
              paths.database_file.empty(),
          "a failed resolution should not leave stale paths in the output");

    Check(!biblioled::BuildApplicationPaths(
              L"\\\\servidor\\BiblioLED.exe", &paths, &error),
          "an incomplete UNC path without a share and filename must be rejected");
    Check(!biblioled::BuildApplicationPaths(
              L"C:\\folder\\", &paths, &error),
          "a path ending in a separator must be rejected as a directory");
}

void TestNullOutput()
{
    std::wstring error;
    Check(!biblioled::BuildApplicationPaths(
              L"C:\\BiblioLED.exe", NULL, &error),
          "a null output pointer must be rejected");
    Check(!error.empty(), "a null output pointer should produce an error");
}

} // namespace

int main()
{
    TestDrivePath();
    TestDriveRoot();
    TestUncPath();
    TestUnicodeAndForwardSlashes();
    TestInvalidPaths();
    TestNullOutput();

    if (failures != 0) {
        std::cerr << failures << " application-path test(s) failed."
                  << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "All application-path tests passed." << std::endl;
    return EXIT_SUCCESS;
}
