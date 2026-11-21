#include "biblioled/settings_store.h"

#include <cerrno>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace biblioled {
namespace {

const size_t kMaximumSettingsBytes = 64U * 1024U;
const int kTemporaryNameAttempts = 32;

void SetError(std::string* error, const std::string& message)
{
    if (error) *error = message;
}

std::string Trim(const std::string& value)
{
    const std::string::size_type first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const std::string::size_type last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseInteger(const std::string& text, int minimum, int maximum, int* value)
{
    if (!value || text.empty() || text.find('\0') != std::string::npos) return false;
    errno = 0;
    char* end = NULL;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno == ERANGE || !end || end == text.c_str() ||
        *end != '\0' || parsed < minimum || parsed > maximum) return false;
    *value = static_cast<int>(parsed);
    return true;
}

bool ParseIntegerList(const std::string& text, int minimum, int maximum,
                      int* values, int count, bool unique)
{
    if (!values || count <= 0) return false;
    std::string::size_type start = 0;
    for (int i = 0; i < count; ++i) {
        const std::string::size_type comma = text.find(',', start);
        if ((i < count - 1 && comma == std::string::npos) ||
            (i == count - 1 && comma != std::string::npos)) return false;
        const std::string token = Trim(text.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!ParseInteger(token, minimum, maximum, &values[i])) return false;
        if (unique) {
            for (int j = 0; j < i; ++j) {
                if (values[j] == values[i]) return false;
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return true;
}

bool ParseBoolean(const std::string& text, bool* value)
{
    if (!value) return false;
    if (text == "0") { *value = false; return true; }
    if (text == "1") { *value = true; return true; }
    return false;
}

std::string IntegerListToString(const int* values, int count)
{
    std::ostringstream output;
    for (int i = 0; i < count; ++i) {
        if (i) output << ',';
        output << values[i];
    }
    return output.str();
}

bool ValidateSettings(const AppSettings& s, std::string* error)
{
    if (s.daily_limit < 1 || s.daily_limit > 1000 ||
        s.monthly_limit < 1 || s.monthly_limit > 10000 ||
        s.monthly_window_days < 1 || s.monthly_window_days > 3660 ||
        s.loan_duration_days < 1 || s.loan_duration_days > 3660) {
        SetError(error, "Os limites e a duração têm valores fora do intervalo permitido.");
        return false;
    }
    if (s.audit_log_max_size_bytes < 65536UL ||
        s.audit_log_max_size_bytes > 1073741824UL) {
        SetError(error, "O tamanho máximo do registo de auditoria é inválido.");
        return false;
    }
    if ((s.backup_compression_format != "gzip" &&
         s.backup_compression_format != "zlib") ||
        s.backup_compression_level < 0 || s.backup_compression_level > 9) {
        SetError(error, "As definições de compressão são inválidas.");
        return false;
    }

    bool seen[kSettingsColumnCount] = {false};
    for (int i = 0; i < kSettingsColumnCount; ++i) {
        const int column = s.column_order[i];
        if (column < 0 || column >= kSettingsColumnCount || seen[column]) {
            SetError(error, "A ordem das colunas tem de conter cada coluna uma única vez.");
            return false;
        }
        seen[column] = true;
        if (s.column_widths[i] < 30 || s.column_widths[i] > 2000) {
            SetError(error, "A largura de uma coluna está fora do intervalo permitido.");
            return false;
        }
    }
    return true;
}

bool ParseSettingsText(const std::string& contents,
                       AppSettings* settings,
                       std::string* error)
{
    if (!settings) {
        SetError(error, "O destino das definições não foi indicado.");
        return false;
    }
    *settings = AppSettings();
    if (contents.size() > kMaximumSettingsBytes ||
        contents.find('\0') != std::string::npos) {
        SetError(error, "O ficheiro de definições excede o tamanho permitido ou contém bytes nulos.");
        return false;
    }

    std::string text = contents;
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) text.erase(0, 3);

    AppSettings loaded;
    std::istringstream input(text);
    std::string line;
    std::set<std::string> seen;
    bool daily = false, monthly = false, window = false, duration = false;
    bool valid = true;

    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        const std::string::size_type equals = line.find('=');
        if (equals == std::string::npos) { valid = false; break; }

        const std::string key = Trim(line.substr(0, equals));
        const std::string value = Trim(line.substr(equals + 1));
        int parsed = 0;

        if (key == "daily_limit") {
            if (!seen.insert(key).second) { valid = false; break; }
            valid = ParseInteger(value, 1, 1000, &parsed);
            if (valid) loaded.daily_limit = parsed;
            daily = true;
        } else if (key == "monthly_limit") {
            if (!seen.insert(key).second) { valid = false; break; }
            valid = ParseInteger(value, 1, 10000, &parsed);
            if (valid) loaded.monthly_limit = parsed;
            monthly = true;
        } else if (key == "monthly_window_days") {
            if (!seen.insert(key).second) { valid = false; break; }
            valid = ParseInteger(value, 1, 3660, &parsed);
            if (valid) loaded.monthly_window_days = parsed;
            window = true;
        } else if (key == "loan_duration_days") {
            if (!seen.insert(key).second) { valid = false; break; }
            valid = ParseInteger(value, 1, 3660, &parsed);
            if (valid) loaded.loan_duration_days = parsed;
            duration = true;
        } else if (key == "enabled") {
            if (!seen.insert(key).second) { valid = false; break; }
            valid = ParseBoolean(value, &loaded.diagnostics_enabled);
        } else if (key == "max_log_size_bytes") {
            if (!seen.insert(key).second) { valid = false; break; }
            int size = 0;
            valid = ParseInteger(value, 65536, 1073741824, &size);
            if (valid) loaded.audit_log_max_size_bytes = static_cast<unsigned long>(size);
        } else if (key == "compression_format") {
            if (!seen.insert(key).second) { valid = false; break; }
            valid = value == "gzip" || value == "zlib";
            if (valid) loaded.backup_compression_format = value;
        } else if (key == "compression_level") {
            if (!seen.insert(key).second) { valid = false; break; }
            valid = ParseInteger(value, 0, 9, &parsed);
            if (valid) loaded.backup_compression_level = parsed;
        } else if (key == "order") {
            if (!seen.insert(key).second) { valid = false; break; }
            valid = ParseIntegerList(value, 0, kSettingsColumnCount - 1,
                                     loaded.column_order, kSettingsColumnCount, true);
        } else if (key == "widths") {
            if (!seen.insert(key).second) { valid = false; break; }
            valid = ParseIntegerList(value, 30, 2000,
                                     loaded.column_widths, kSettingsColumnCount, false);
        }

        if (!valid) break;
    }

    if (!valid || !daily || !monthly || !window || !duration) {
        SetError(error,
            "As configurações são inválidas. O ficheiro original foi preservado; "
            "serão usados valores predefinidos nesta sessão.");
        *settings = AppSettings();
        return false;
    }
    *settings = loaded;
    return true;
}

std::string SerializeSettings(const AppSettings& s)
{
    std::ostringstream out;
    out << "; BiblioLED portable settings\r\n"
           "[limits]\r\n"
        << "daily_limit=" << s.daily_limit << "\r\n"
        << "monthly_limit=" << s.monthly_limit << "\r\n"
        << "monthly_window_days=" << s.monthly_window_days << "\r\n"
        << "loan_duration_days=" << s.loan_duration_days << "\r\n\r\n"
           "[diagnostics]\r\n"
        << "enabled=" << (s.diagnostics_enabled ? 1 : 0) << "\r\n"
        << "max_log_size_bytes=" << s.audit_log_max_size_bytes << "\r\n\r\n"
           "[backups]\r\n"
        << "compression_format=" << s.backup_compression_format << "\r\n"
        << "compression_level=" << s.backup_compression_level << "\r\n\r\n"
           "[columns]\r\n"
        << "order=" << IntegerListToString(s.column_order, kSettingsColumnCount) << "\r\n"
        << "widths=" << IntegerListToString(s.column_widths, kSettingsColumnCount) << "\r\n";
    return out.str();
}

#ifdef _WIN32

bool Utf8PathToWide(const std::string& path, std::wstring* result, std::string* error)
{
    if (!result || path.empty() || path.find('\0') != std::string::npos) {
        SetError(error, "O caminho do ficheiro de definições é inválido.");
        return false;
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    if (length <= 1) {
        SetError(error, "Não foi possível converter o caminho UTF-8 das definições.");
        return false;
    }
    std::wstring value(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &value[0], length)) {
        SetError(error, "Não foi possível converter o caminho UTF-8 das definições.");
        return false;
    }
    value.resize(static_cast<size_t>(length - 1));
    *result = value;
    return true;
}

bool ReadSettingsBytes(const std::wstring& path, std::string* contents,
                       bool* exists, bool* invalid_file, std::string* error)
{
    if (invalid_file) *invalid_file = false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            *exists = false;
            contents->clear();
            return true;
        }
        std::ostringstream message;
        message << "Não foi possível abrir config/settings.ini (erro Win32 "
                << code << ").";
        SetError(error, message.str());
        return false;
    }

    *exists = true;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        SetError(error, "Não foi possível obter o tamanho de config/settings.ini.");
        return false;
    }
    if (static_cast<unsigned long long>(size.QuadPart) > kMaximumSettingsBytes) {
        CloseHandle(file);
        if (invalid_file) *invalid_file = true;
        SetError(error, "config/settings.ini excede o limite de 64 KiB.");
        return false;
    }

    bool ok = true;
    char buffer[8192];
    while (ok) {
        DWORD received = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &received, NULL)) {
            ok = false;
            break;
        }
        if (received == 0) break;
        if (contents->size() + received > kMaximumSettingsBytes) {
            ok = false;
            break;
        }
        contents->append(buffer, received);
    }
    if (!CloseHandle(file)) ok = false;
    if (!ok) {
        contents->clear();
        SetError(error, "Não foi possível ler integralmente config/settings.ini.");
    }
    return ok;
}

bool WriteSettingsBytesAtomically(const std::wstring& path,
                                  const std::string& contents,
                                  bool replace_existing,
                                  std::string* error)
{
    const DWORD process_id = GetCurrentProcessId();
    std::wstring temporary_path;
    HANDLE file = INVALID_HANDLE_VALUE;

    for (int attempt = 0; attempt < kTemporaryNameAttempts; ++attempt) {
        std::wostringstream name;
        name << path << L".tmp." << process_id << L"." << attempt;
        temporary_path = name.str();
        file = CreateFileW(temporary_path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
            std::ostringstream message;
            message << "Não foi possível criar o ficheiro temporário das definições (erro Win32 "
                    << code << ").";
            SetError(error, message.str());
            return false;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, "Não foi possível reservar um nome temporário livre para as definições.");
        return false;
    }

    size_t offset = 0;
    bool ok = true;
    while (offset < contents.size()) {
        const DWORD requested = static_cast<DWORD>(
            contents.size() - offset > 65536U ? 65536U : contents.size() - offset);
        DWORD written = 0;
        if (!WriteFile(file, contents.data() + offset, requested, &written, NULL) ||
            written == 0) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(file)) ok = false;
    if (!CloseHandle(file)) ok = false;
    if (!ok) {
        DeleteFileW(temporary_path.c_str());
        SetError(error, "Não foi possível gravar e descarregar integralmente o ficheiro temporário das definições.");
        return false;
    }

    const DWORD flags = MOVEFILE_WRITE_THROUGH |
        (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temporary_path.c_str(), path.c_str(), flags)) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary_path.c_str());
        std::ostringstream message;
        message << "Não foi possível instalar config/settings.ini (erro Win32 "
                << code << "). O ficheiro anterior foi preservado.";
        SetError(error, message.str());
        return false;
    }
    return true;
}

#else

bool ReadSettingsBytes(const std::string& path, std::string* contents,
                       bool* exists, bool* invalid_file, std::string* error)
{
    if (invalid_file) *invalid_file = false;
    const int file = open(path.c_str(), O_RDONLY);
    if (file < 0) {
        if (errno == ENOENT) {
            *exists = false;
            contents->clear();
            return true;
        }
        SetError(error, "Não foi possível abrir config/settings.ini.");
        return false;
    }

    *exists = true;
    struct stat status;
    if (fstat(file, &status) != 0) {
        close(file);
        SetError(error, "Não foi possível obter os metadados de config/settings.ini.");
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        close(file);
        SetError(error, "config/settings.ini não é um ficheiro regular.");
        return false;
    }
    if (static_cast<unsigned long long>(status.st_size) > kMaximumSettingsBytes) {
        close(file);
        if (invalid_file) *invalid_file = true;
        SetError(error, "config/settings.ini excede o limite de 64 KiB.");
        return false;
    }

    char buffer[8192];
    bool ok = true;
    for (;;) {
        const ssize_t received = read(file, buffer, sizeof(buffer));
        if (received == 0) break;
        if (received < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (contents->size() + static_cast<size_t>(received) > kMaximumSettingsBytes) {
            ok = false;
            break;
        }
        contents->append(buffer, static_cast<size_t>(received));
    }
    if (close(file) != 0) ok = false;
    if (!ok) {
        contents->clear();
        SetError(error, "Não foi possível ler integralmente config/settings.ini.");
    }
    return ok;
}

bool WriteSettingsBytesAtomically(const std::string& path,
                                  const std::string& contents,
                                  bool replace_existing,
                                  std::string* error)
{
    const long process_id = static_cast<long>(getpid());
    std::string temporary_path;
    int file = -1;

    for (int attempt = 0; attempt < kTemporaryNameAttempts; ++attempt) {
        std::ostringstream name;
        name << path << ".tmp." << process_id << "." << attempt;
        temporary_path = name.str();
        file = open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL,
                    S_IRUSR | S_IWUSR);
        if (file >= 0) break;
        if (errno != EEXIST) {
            SetError(error, "Não foi possível criar o ficheiro temporário das definições.");
            return false;
        }
    }
    if (file < 0) {
        SetError(error, "Não foi possível reservar um nome temporário livre para as definições.");
        return false;
    }

    size_t offset = 0;
    bool ok = true;
    while (offset < contents.size()) {
        const ssize_t written = write(file, contents.data() + offset,
                                      contents.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (written == 0) { ok = false; break; }
        offset += static_cast<size_t>(written);
    }
    if (ok && fsync(file) != 0) ok = false;
    if (close(file) != 0) ok = false;
    if (!ok) {
        unlink(temporary_path.c_str());
        SetError(error, "Não foi possível gravar e descarregar integralmente o ficheiro temporário das definições.");
        return false;
    }

    int result = 0;
    if (replace_existing) {
        result = rename(temporary_path.c_str(), path.c_str());
    } else {
        result = link(temporary_path.c_str(), path.c_str());
        if (result == 0) unlink(temporary_path.c_str());
    }
    if (result != 0) {
        unlink(temporary_path.c_str());
        SetError(error, "Não foi possível instalar config/settings.ini. O ficheiro anterior foi preservado.");
        return false;
    }
    return true;
}

#endif

} // namespace

bool LoadSettingsFile(const std::string& utf8_path,
                      AppSettings* settings,
                      std::string* error,
                      SettingsLoadStatus* status)
{
    if (status) *status = kSettingsLoadError;
    if (error) error->clear();
    if (!settings) {
        SetError(error, "O destino das definições não foi indicado.");
        return false;
    }
    *settings = AppSettings();
    if (utf8_path.empty()) {
        SetError(error, "O caminho de config/settings.ini está vazio.");
        return false;
    }

    std::string contents;
    bool exists = false;
    bool invalid_file = false;
#ifdef _WIN32
    std::wstring path;
    if (!Utf8PathToWide(utf8_path, &path, error) ||
        !ReadSettingsBytes(path, &contents, &exists, &invalid_file, error)) {
#else
    if (!ReadSettingsBytes(utf8_path, &contents, &exists, &invalid_file, error)) {
#endif
        if (invalid_file) {
            if (status) *status = kSettingsLoadInvalidFile;
        }
        return false;
    }

    if (!exists) {
        const AppSettings defaults;
        const std::string text = SerializeSettings(defaults);
#ifdef _WIN32
        if (!WriteSettingsBytesAtomically(path, text, false, error)) return false;
#else
        if (!WriteSettingsBytesAtomically(utf8_path, text, false, error)) return false;
#endif
        *settings = defaults;
        if (status) *status = kSettingsLoadCreatedDefaults;
        return true;
    }

    if (!ParseSettingsText(contents, settings, error)) {
        if (status) *status = kSettingsLoadInvalidFile;
        return false;
    }

    if (status) *status = kSettingsLoadLoaded;
    return true;
}

bool LoadSettingsFile(const std::string& utf8_path,
                      AppSettings* settings,
                      std::string* error)
{
    return LoadSettingsFile(utf8_path, settings, error, NULL);
}

bool SaveSettingsFile(const std::string& utf8_path,
                      const AppSettings& settings,
                      std::string* error)
{
    if (error) error->clear();
    if (utf8_path.empty()) {
        SetError(error, "O caminho de config/settings.ini está vazio.");
        return false;
    }
    if (!ValidateSettings(settings, error)) return false;

    // Enforce the invalid-file preservation rule at the storage boundary,
    // not only in the current Win32 callers. Also detect concurrent creation
    // rather than replacing a file that appeared after this existence check.
    std::string current_contents;
    bool exists = false;
#ifdef _WIN32
    std::wstring path;
    if (!Utf8PathToWide(utf8_path, &path, error) ||
        !ReadSettingsBytes(path, &current_contents, &exists, NULL, error)) {
        return false;
    }
#else
    if (!ReadSettingsBytes(utf8_path, &current_contents, &exists, NULL, error)) {
        return false;
    }
#endif

    if (exists) {
        AppSettings current_settings;
        if (!ParseSettingsText(current_contents, &current_settings, error)) {
            return false;
        }
    }

    const std::string text = SerializeSettings(settings);
#ifdef _WIN32
    return WriteSettingsBytesAtomically(path, text, exists, error);
#else
    return WriteSettingsBytesAtomically(utf8_path, text, exists, error);
#endif
}

bool ArchiveInvalidSettingsFile(const std::string& utf8_path,
                                std::string* archived_utf8_path,
                                std::string* error)
{
    if (error) error->clear();
    if (archived_utf8_path) archived_utf8_path->clear();
    if (!archived_utf8_path || utf8_path.empty()) {
        SetError(error, "O caminho ou o destino do arquivo de definições é inválido.");
        return false;
    }

    std::string contents;
    bool exists = false;
    bool invalid_file = false;
#ifdef _WIN32
    std::wstring source_path;
    if (!Utf8PathToWide(utf8_path, &source_path, error)) return false;
    if (!ReadSettingsBytes(source_path, &contents, &exists,
                           &invalid_file, error) && !invalid_file) {
        return false;
    }
#else
    if (!ReadSettingsBytes(utf8_path, &contents, &exists,
                           &invalid_file, error) && !invalid_file) {
        return false;
    }
#endif

    if (!exists) {
        SetError(error, "O ficheiro de definições não existe.");
        return false;
    }

    if (!invalid_file) {
        AppSettings parsed;
        if (ParseSettingsText(contents, &parsed, NULL)) {
            SetError(error, "O ficheiro de definições é válido e não precisa de ser arquivado.");
            return false;
        }
        invalid_file = true;
    }

    const std::string archived_path = utf8_path + ".invalid";
#ifdef _WIN32
    std::wstring destination_path = source_path + L".invalid";
    if (!MoveFileExW(source_path.c_str(), destination_path.c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::ostringstream message;
        message << "Não foi possível arquivar o ficheiro inválido como .invalid (erro Win32 "
                << code << "). O original foi preservado; confirme se o arquivo já existe.";
        SetError(error, message.str());
        return false;
    }
#else
    // link() creates the destination exclusively. If it already exists, the
    // original is untouched. If unlink fails, both copies remain available.
    if (link(utf8_path.c_str(), archived_path.c_str()) != 0) {
        SetError(error,
            "Não foi possível criar o arquivo .invalid. O original foi preservado; confirme se o arquivo já existe.");
        return false;
    }
    if (unlink(utf8_path.c_str()) != 0) {
        SetError(error,
            "O arquivo .invalid foi criado, mas não foi possível remover o nome original. Ambas as cópias foram preservadas.");
        return false;
    }
#endif

    if (error) error->clear();
    *archived_utf8_path = archived_path;
    return true;
}

} // namespace biblioled
