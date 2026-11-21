#include "biblioled/application_paths.h"

namespace biblioled {
namespace {

bool IsSeparator(wchar_t value)
{
    return value == L'\\' || value == L'/';
}

bool IsAsciiLetter(wchar_t value)
{
    return (value >= L'A' && value <= L'Z') ||
           (value >= L'a' && value <= L'z');
}

bool IsDriveAbsolutePath(const std::wstring& path)
{
    return path.size() >= 3 &&
           IsAsciiLetter(path[0]) &&
           path[1] == L':' &&
           IsSeparator(path[2]);
}

bool IsUncAbsolutePath(const std::wstring& path)
{
    if (path.size() < 5 ||
        !IsSeparator(path[0]) ||
        !IsSeparator(path[1])) {
        return false;
    }

    const std::wstring::size_type server_end =
        path.find_first_of(L"\\/", 2);
    if (server_end == std::wstring::npos || server_end == 2 ||
        server_end + 1 >= path.size()) {
        return false;
    }

    const std::wstring::size_type share_end =
        path.find_first_of(L"\\/", server_end + 1);
    return share_end != std::wstring::npos &&
           share_end > server_end + 1;
}

std::wstring JoinWindowsPath(const std::wstring& directory,
                             const wchar_t* child)
{
    if (directory.empty()) return std::wstring(child);

    const wchar_t last = directory[directory.size() - 1];
    if (IsSeparator(last)) return directory + child;

    return directory + L"\\" + child;
}

void SetError(std::wstring* error, const wchar_t* message)
{
    if (error) *error = message;
}

} // namespace

bool BuildApplicationPaths(const std::wstring& executable_path,
                           ApplicationPaths* paths,
                           std::wstring* error)
{
    if (error) error->clear();
    if (!paths) {
        SetError(error, L"O destino dos caminhos da aplicação não foi indicado.");
        return false;
    }

    *paths = ApplicationPaths();

    if (executable_path.empty()) {
        SetError(error, L"O caminho do executável está vazio.");
        return false;
    }

    if (!IsDriveAbsolutePath(executable_path) &&
        !IsUncAbsolutePath(executable_path)) {
        SetError(error,
            L"O caminho do executável tem de ser absoluto (unidade local ou pasta de rede).");
        return false;
    }

    if (IsSeparator(executable_path[executable_path.size() - 1])) {
        SetError(error, L"O caminho do executável não contém um nome de ficheiro.");
        return false;
    }

    const std::wstring::size_type separator =
        executable_path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        SetError(error, L"O caminho do executável não contém uma pasta.");
        return false;
    }

    std::wstring root;
    if (IsDriveAbsolutePath(executable_path) && separator == 2) {
        // Preserve the root separator for an installation directly on C:\.
        root = executable_path.substr(0, 3);
    } else {
        root = executable_path.substr(0, separator);
    }

    if (root.empty()) {
        SetError(error, L"Não foi possível determinar a pasta da aplicação.");
        return false;
    }

    // Accept either input separator but consistently generate Win32 paths.
    for (size_t i = 0; i < root.size(); ++i) {
        if (root[i] == L'/') root[i] = L'\\';
    }

    ApplicationPaths resolved;
    resolved.application_directory = root;
    resolved.config_directory = JoinWindowsPath(root, L"config");
    resolved.data_directory = JoinWindowsPath(root, L"data");
    resolved.backup_directory =
        JoinWindowsPath(resolved.data_directory, L"backups");
    resolved.export_directory = JoinWindowsPath(root, L"exports");
    resolved.log_directory = JoinWindowsPath(root, L"logs");
    resolved.settings_file =
        JoinWindowsPath(resolved.config_directory, L"settings.ini");
    resolved.database_file =
        JoinWindowsPath(resolved.data_directory, L"biblioteca.sqlite");
    resolved.database_marker_file =
        JoinWindowsPath(resolved.config_directory, L"database.initialized");

    *paths = resolved;
    return true;
}

} // namespace biblioled
