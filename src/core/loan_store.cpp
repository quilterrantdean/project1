#include "biblioled/loan_store.h"
#include "biblioled/loan_rules.h"
#include "biblioled/backup_compression.h"
#include "biblioled/backup_retention.h"

#include <sqlite3.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace biblioled {
namespace {

bool CheckSqliteResult(sqlite3* db, int actual, int expected,
                       const char* operation, std::string* error)
{
    if (actual == expected) return true;
    if (error) {
        *error = operation;
        *error += ": ";
        *error += db ? sqlite3_errmsg(db) : "SQLite database is not open";
    }
    return false;
}

bool FinalizeSqliteStatement(sqlite3* db, sqlite3_stmt* statement,
                             const char* operation, std::string* error)
{
    return CheckSqliteResult(
        db, sqlite3_finalize(statement), SQLITE_OK, operation, error);
}

bool FileExists(const std::string& path)
{
#ifdef _WIN32
    // LoanStore paths are UTF-8 at the public boundary. The narrow
    // std::ifstream overload uses the active Windows code page instead.
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    if (length <= 1) return false;

    std::wstring wide_path(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(
            CP_UTF8, 0, path.c_str(), -1, &wide_path[0], length)) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(wide_path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
    return file.good();
#endif
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

bool HasExpectedSchema(sqlite3* db)
{
    struct ExpectedColumn {
        const char* name;
        const char* declared_type;
        int not_null;
        int primary_key;
    };
    static const ExpectedColumn expected[] = {
        {"id", "INTEGER", 0, 1},
        {"title", "TEXT", 1, 0},
        {"loan_date", "TEXT", 1, 0},
        {"type", "TEXT", 1, 0}
    };

    sqlite3_stmt* statement = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(loans);",
                           -1, &statement, NULL) != SQLITE_OK) {
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    bool found[sizeof(expected) / sizeof(expected[0])] = {false};
    bool valid = true;
    size_t column_count = 0;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        ++column_count;
        const unsigned char* name = sqlite3_column_text(statement, 1);
        const unsigned char* declared_type = sqlite3_column_text(statement, 2);
        if (!name || !declared_type) {
            valid = false;
            continue;
        }

        size_t match = sizeof(expected) / sizeof(expected[0]);
        for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
            if (sqlite3_stricmp(reinterpret_cast<const char*>(name),
                                expected[i].name) == 0) {
                match = i;
                break;
            }
        }
        if (match == sizeof(expected) / sizeof(expected[0]) || found[match]) {
            valid = false;
            continue;
        }

        found[match] = true;
        if (sqlite3_stricmp(reinterpret_cast<const char*>(declared_type),
                            expected[match].declared_type) != 0 ||
            sqlite3_column_int(statement, 3) != expected[match].not_null ||
            sqlite3_column_int(statement, 5) != expected[match].primary_key) {
            valid = false;
        }
    }

    const int finalize_rc = sqlite3_finalize(statement);
    if (step != SQLITE_DONE || finalize_rc != SQLITE_OK ||
        !valid || column_count != sizeof(expected) / sizeof(expected[0])) {
        return false;
    }
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        if (!found[i]) return false;
    }
    return true;
}

bool HasAuditTable(sqlite3* db, bool* exists, std::string* error)
{
    if (exists) *exists = false;
    sqlite3_stmt* statement = NULL;
    const char* sql =
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'audit_events';";
    const int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(db);
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    bool found = false;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        found = true;
    }
    const int finalize_rc = sqlite3_finalize(statement);
    if (step != SQLITE_DONE || finalize_rc != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(db);
        return false;
    }

    if (exists) *exists = found;
    return true;
}

bool HasAuditSchema(sqlite3* db)
{
    struct ExpectedColumn {
        const char* name;
        const char* declared_type;
        int not_null;
        int primary_key;
    };
    static const ExpectedColumn expected[] = {
        {"id", "INTEGER", 0, 1},
        {"occurred_at", "TEXT", 1, 0},
        {"action", "TEXT", 1, 0},
        {"loan_id", "INTEGER", 0, 0},
        {"before_title", "TEXT", 0, 0},
        {"before_loan_date", "TEXT", 0, 0},
        {"before_type", "TEXT", 0, 0},
        {"after_title", "TEXT", 0, 0},
        {"after_loan_date", "TEXT", 0, 0},
        {"after_type", "TEXT", 0, 0},
        {"details", "TEXT", 1, 0}
    };

    sqlite3_stmt* statement = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(audit_events);",
                           -1, &statement, NULL) != SQLITE_OK) {
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    bool found[sizeof(expected) / sizeof(expected[0])] = {false};
    bool valid = true;
    size_t column_count = 0;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        ++column_count;
        const unsigned char* name = sqlite3_column_text(statement, 1);
        const unsigned char* declared_type = sqlite3_column_text(statement, 2);
        if (!name || !declared_type) {
            valid = false;
            continue;
        }

        size_t match = sizeof(expected) / sizeof(expected[0]);
        for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
            if (sqlite3_stricmp(reinterpret_cast<const char*>(name),
                                expected[i].name) == 0) {
                match = i;
                break;
            }
        }
        if (match == sizeof(expected) / sizeof(expected[0]) || found[match]) {
            valid = false;
            continue;
        }

        found[match] = true;
        if (sqlite3_stricmp(reinterpret_cast<const char*>(declared_type),
                            expected[match].declared_type) != 0 ||
            sqlite3_column_int(statement, 3) != expected[match].not_null ||
            sqlite3_column_int(statement, 5) != expected[match].primary_key) {
            valid = false;
        }
    }

    const int finalize_rc = sqlite3_finalize(statement);
    if (step != SQLITE_DONE || finalize_rc != SQLITE_OK ||
        !valid || column_count != sizeof(expected) / sizeof(expected[0])) {
        return false;
    }
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        if (!found[i]) return false;
    }
    return true;
}

bool ReadUserVersion(sqlite3* db, int* version, std::string* error)
{
    if (!version) {
        if (error) *error = "Invalid schema-version output argument.";
        return false;
    }

    sqlite3_stmt* statement = NULL;
    const int rc = sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(db);
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    const int first_step = sqlite3_step(statement);
    if (first_step != SQLITE_ROW) {
        const std::string detail = sqlite3_errmsg(db);
        const int finalize_rc = sqlite3_finalize(statement);
        if (error) {
            *error = detail;
            if (finalize_rc != SQLITE_OK) {
                *error += " (SQLite statement finalisation also failed.)";
            }
        }
        return false;
    }

    const int value = sqlite3_column_int(statement, 0);
    const int terminal_step = sqlite3_step(statement);
    const int finalize_rc = sqlite3_finalize(statement);
    if (terminal_step != SQLITE_DONE || finalize_rc != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(db);
        return false;
    }

    *version = value;
    return true;
}

bool ExecuteSql(sqlite3* db, const char* sql, std::string* error)
{
    char* message = NULL;
    const int rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (rc == SQLITE_OK) return true;

    const std::string detail = message ? message : sqlite3_errmsg(db);
    sqlite3_free(message);
    if (error) *error = detail;
    return false;
}

std::string CurrentTimestamp()
{
    const std::time_t now = std::time(NULL);
    const std::tm* local = std::localtime(&now);
    if (!local) return std::string();

    char buffer[32] = {0};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", local) == 0) {
        return std::string();
    }
    return buffer;
}

bool BindOptionalText(sqlite3* db, sqlite3_stmt* statement, int index,
                      const std::string* value, std::string* error)
{
    const int rc = value
        ? sqlite3_bind_text(statement, index, value->c_str(), -1, SQLITE_TRANSIENT)
        : sqlite3_bind_null(statement, index);
    return CheckSqliteResult(db, rc, SQLITE_OK,
                             "Unable to bind an audit-event value", error);
}

bool InsertAuditEvent(sqlite3* db,
                      const char* action,
                      int loan_id,
                      const LoanRecord* before,
                      const LoanRecord* after,
                      const std::string& details,
                      std::string* error)
{
    const char* sql =
        "INSERT INTO audit_events ("
        "occurred_at, action, loan_id, "
        "before_title, before_loan_date, before_type, "
        "after_title, after_loan_date, after_type, details"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);";

    sqlite3_stmt* statement = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(db);
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    const std::string timestamp = CurrentTimestamp();
    if (timestamp.empty()) {
        if (error) *error = "Unable to obtain the local timestamp for the audit event.";
        sqlite3_finalize(statement);
        return false;
    }
    bool ok =
        CheckSqliteResult(db,
            sqlite3_bind_text(statement, 1, timestamp.c_str(), -1, SQLITE_TRANSIENT),
            SQLITE_OK, "Unable to bind audit timestamp", error) &&
        CheckSqliteResult(db,
            sqlite3_bind_text(statement, 2, action, -1, SQLITE_TRANSIENT),
            SQLITE_OK, "Unable to bind audit action", error);
    if (ok) {
        rc = loan_id > 0
            ? sqlite3_bind_int(statement, 3, loan_id)
            : sqlite3_bind_null(statement, 3);
        ok = CheckSqliteResult(db, rc, SQLITE_OK,
                               "Unable to bind audit loan identifier", error);
    }

    if (ok) ok = BindOptionalText(db, statement, 4,
                                  before ? &before->title : NULL, error);
    if (ok) ok = BindOptionalText(db, statement, 5,
                                  before ? &before->loan_date : NULL, error);
    if (ok) ok = BindOptionalText(db, statement, 6,
                                  before ? &before->type : NULL, error);
    if (ok) ok = BindOptionalText(db, statement, 7,
                                  after ? &after->title : NULL, error);
    if (ok) ok = BindOptionalText(db, statement, 8,
                                  after ? &after->loan_date : NULL, error);
    if (ok) ok = BindOptionalText(db, statement, 9,
                                  after ? &after->type : NULL, error);
    if (ok) {
        rc = sqlite3_bind_text(statement, 10, details.c_str(), -1, SQLITE_TRANSIENT);
        ok = CheckSqliteResult(db, rc, SQLITE_OK,
                               "Unable to bind audit details", error);
    }

    if (ok) {
        rc = sqlite3_step(statement);
        ok = CheckSqliteResult(db, rc, SQLITE_DONE,
                               "Unable to insert audit event", error);
    }
    if (ok) {
        ok = FinalizeSqliteStatement(db, statement,
                                     "Unable to finalise audit insertion", error);
    } else {
        sqlite3_finalize(statement);
    }
    return ok;
}

bool ReadLoanById(sqlite3* db,
                  int id,
                  LoanRecord* record,
                  bool* found,
                  std::string* error)
{
    if (found) *found = false;
    sqlite3_stmt* statement = NULL;
    const char* sql =
        "SELECT id, title, loan_date, type FROM loans WHERE id = ?1;";
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(db);
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    rc = sqlite3_bind_int(statement, 1, id);
    if (!CheckSqliteResult(db, rc, SQLITE_OK,
                           "Unable to bind loan identifier", error)) {
        sqlite3_finalize(statement);
        return false;
    }

    rc = sqlite3_step(statement);
    bool row_found = false;
    if (rc == SQLITE_ROW) {
        row_found = true;
        if (record) {
            record->id = sqlite3_column_int(statement, 0);
            const unsigned char* title = sqlite3_column_text(statement, 1);
            const unsigned char* loan_date = sqlite3_column_text(statement, 2);
            const unsigned char* type = sqlite3_column_text(statement, 3);
            record->title = title ? reinterpret_cast<const char*>(title) : "";
            record->loan_date = loan_date ? reinterpret_cast<const char*>(loan_date) : "";
            record->type = type ? reinterpret_cast<const char*>(type) : "";
        }
        rc = sqlite3_step(statement);
    }
    if (rc != SQLITE_DONE) {
        if (error) *error = std::string("Unable to read loan record: ") + sqlite3_errmsg(db);
        sqlite3_finalize(statement);
        return false;
    }

    if (!FinalizeSqliteStatement(
            db, statement, "Unable to finalise loan lookup", error)) {
        return false;
    }
    if (found) *found = row_found;
    return true;
}

bool CreateAuditSchema(sqlite3* db, std::string* error)
{
    static const char* sql =
        "CREATE TABLE audit_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " occurred_at TEXT NOT NULL,"
        " action TEXT NOT NULL,"
        " loan_id INTEGER,"
        " before_title TEXT,"
        " before_loan_date TEXT,"
        " before_type TEXT,"
        " after_title TEXT,"
        " after_loan_date TEXT,"
        " after_type TEXT,"
        " details TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE INDEX idx_audit_events_occurred_at "
        " ON audit_events(occurred_at, id);"
        "CREATE INDEX idx_audit_events_loan_id "
        " ON audit_events(loan_id, id);";
    return ExecuteSql(db, sql, error);
}

bool CopyDatabase(sqlite3* source, sqlite3* destination, std::string* error)
{
    sqlite3_backup* backup = sqlite3_backup_init(destination, "main", source, "main");
    if (!backup) {
        if (error) *error = sqlite3_errmsg(destination);
        return false;
    }

    int rc = SQLITE_OK;
    int busy_attempts = 0;
    while (true) {
        rc = sqlite3_backup_step(backup, -1);
        if (rc == SQLITE_DONE) break;
        if (rc == SQLITE_OK) continue;
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            if (++busy_attempts >= 200) break;
            sqlite3_sleep(25);
            continue;
        }
        break;
    }

    const int finish_rc = sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE || finish_rc != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(destination);
        return false;
    }
    return true;
}

bool ValidateDatabaseFile(const std::string& path, std::string* error)
{
    sqlite3* check_db = NULL;
    int rc = sqlite3_open_v2(path.c_str(), &check_db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        if (error) {
            *error = check_db ? sqlite3_errmsg(check_db) : "Could not open backup for validation.";
        }
        if (check_db) sqlite3_close(check_db);
        return false;
    }

    sqlite3_stmt* statement = NULL;
    rc = sqlite3_prepare_v2(check_db, "PRAGMA integrity_check;", -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(check_db);
        if (statement) sqlite3_finalize(statement);
        sqlite3_close(check_db);
        return false;
    }

    bool saw_result = false;
    bool valid = true;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        saw_result = true;
        const unsigned char* result = sqlite3_column_text(statement, 0);
        if (!result || sqlite3_stricmp(reinterpret_cast<const char*>(result), "ok") != 0) {
            valid = false;
            if (error) {
                *error = result
                    ? reinterpret_cast<const char*>(result)
                    : "SQLite integrity_check returned an empty result.";
            }
        }
    }

    if (rc != SQLITE_DONE) {
        valid = false;
        if (error) *error = sqlite3_errmsg(check_db);
    }
    if (!saw_result) {
        valid = false;
        if (error) *error = "SQLite integrity_check returned no result.";
    }

    // Integrity alone is not enough when reusing an existing daily snapshot:
    // it must also be a supported BiblioLED database, not an unrelated SQLite file.
    if (!HasExpectedSchema(check_db)) {
        valid = false;
        if (error) *error = "The backup does not contain the expected BiblioLED loans table.";
    } else {
        int version = -1;
        bool has_audit = false;
        if (!ReadUserVersion(check_db, &version, error) ||
            !HasAuditTable(check_db, &has_audit, error)) {
            valid = false;
        } else if (version < 0 || version > 1 ||
                   (version == 0 && has_audit) ||
                   (version == 1 && (!has_audit || !HasAuditSchema(check_db)))) {
            valid = false;
            if (error) *error = "The backup has an unsupported or inconsistent BiblioLED schema.";
        }
    }

    const int finalize_rc = sqlite3_finalize(statement);
    if (finalize_rc != SQLITE_OK) {
        valid = false;
        if (error) *error = std::string("Could not finalise backup integrity check: ") + sqlite3_errmsg(check_db);
    }
    const int close_rc = sqlite3_close(check_db);
    if (close_rc != SQLITE_OK) {
        valid = false;
        if (error) *error = "Could not close the backup after validation.";
    }
    return valid;
}

bool HasTemporaryDatabaseFiles(const std::string& path)
{
    return FileExists(path) ||
           FileExists(path + "-journal") ||
           FileExists(path + "-wal") ||
           FileExists(path + "-shm");
}

std::string MakeTemporaryBackupPath(const std::string& base,
                                    const std::string& purpose)
{
    for (int suffix = 0; suffix < 1000; ++suffix) {
        std::ostringstream name;
        name << base << "." << purpose << "_" << suffix << ".tmp";
        const std::string candidate = name.str();
        // Some temporary files are opened by SQLite after decompression or
        // snapshot creation. Never reuse a basename with an orphaned sidecar.
        if (!HasTemporaryDatabaseFiles(candidate)) return candidate;
    }
    return std::string();
}

bool HasSuffix(const std::string& value, const char* suffix)
{
    const std::string ending(suffix);
    return value.size() >= ending.size() &&
           value.compare(value.size() - ending.size(), ending.size(), ending) == 0;
}

void RemoveTemporaryDatabaseFiles(const std::string& path)
{
    RemoveBackupFile(path);
    RemoveBackupFile(path + "-journal");
    RemoveBackupFile(path + "-wal");
    RemoveBackupFile(path + "-shm");
}

bool DecodeBackupFile(const std::string& path,
                      std::string* sqlite_path,
                      std::string* temporary_path,
                      std::string* error)
{
    if (temporary_path) temporary_path->clear();
    if (HasSuffix(path, ".sqlite.gz") || HasSuffix(path, ".gz")) {
        const std::string decoded = MakeTemporaryBackupPath(path, "decoded_sqlite");
        if (decoded.empty()) {
            if (error) *error = "Could not choose a temporary path for backup decompression.";
            return false;
        }
        if (!DecompressBackupFile(path, decoded,
                BackupCompressionFormat::Gzip, error)) {
            RemoveTemporaryDatabaseFiles(decoded);
            return false;
        }
        if (sqlite_path) *sqlite_path = decoded;
        if (temporary_path) *temporary_path = decoded;
        return true;
    }
    if (HasSuffix(path, ".sqlite.zlib") || HasSuffix(path, ".zlib")) {
        const std::string decoded = MakeTemporaryBackupPath(path, "decoded_sqlite");
        if (decoded.empty()) {
            if (error) *error = "Could not choose a temporary path for backup decompression.";
            return false;
        }
        if (!DecompressBackupFile(path, decoded,
                BackupCompressionFormat::Zlib, error)) {
            RemoveTemporaryDatabaseFiles(decoded);
            return false;
        }
        if (sqlite_path) *sqlite_path = decoded;
        if (temporary_path) *temporary_path = decoded;
        return true;
    }
    if (sqlite_path) *sqlite_path = path;
    return true;
}

struct ScopedBackupFile {
    std::string path;
    ~ScopedBackupFile()
    {
        if (!path.empty()) RemoveTemporaryDatabaseFiles(path);
    }
};

BackupCompressionFormat CompressionFormatFromName(const std::string& name)
{
    return name == "zlib" ? BackupCompressionFormat::Zlib
                          : BackupCompressionFormat::Gzip;
}

std::string InvalidBackupPath(const std::string& original, int suffix);

bool PreserveInvalidBackupGroup(const std::string& path,
                               std::string* error)
{
    const std::string sidecar_suffixes[] = {"", "-journal", "-wal", "-shm"};
    bool source_exists[4] = {false, false, false, false};
    for (size_t i = 0; i < 4; ++i) {
        source_exists[i] = FileExists(path + sidecar_suffixes[i]);
    }

    for (int suffix = 0; suffix < 1000; ++suffix) {
        const std::string preserved_path = InvalidBackupPath(path, suffix);
        if (preserved_path.empty()) break;

        bool available = true;
        for (size_t i = 0; i < 4; ++i) {
            if (FileExists(preserved_path + sidecar_suffixes[i])) {
                available = false;
                break;
            }
        }
        if (!available) continue;

        std::vector<std::string> moved_sources;
        std::vector<std::string> moved_destinations;
        for (size_t i = 0; i < 4; ++i) {
            if (!source_exists[i]) continue;
            const std::string source = path + sidecar_suffixes[i];
            const std::string destination = preserved_path + sidecar_suffixes[i];
            if (!MoveBackupFile(source, destination)) {
                bool rollback_ok = true;
                for (size_t moved = moved_sources.size(); moved > 0; --moved) {
                    if (!MoveBackupFile(moved_destinations[moved - 1],
                                        moved_sources[moved - 1])) {
                        rollback_ok = false;
                    }
                }
                if (error) {
                    *error = "A corrupt automatic backup and its SQLite sidecars "
                             "could not all be preserved.";
                    if (!rollback_ok) {
                        *error += " Some files remain under the preservation prefix: ";
                        *error += preserved_path;
                    }
                }
                return false;
            }
            moved_sources.push_back(source);
            moved_destinations.push_back(destination);
        }
        return true;
    }

    if (error) {
        *error = "Could not choose an unused name to preserve a corrupt automatic backup.";
    }
    return false;
}

std::string InvalidBackupPath(const std::string& original, int suffix)
{
    const std::string::size_type separator = original.find_last_of("/\\");
    const std::string directory = separator == std::string::npos
        ? std::string() : original.substr(0, separator + 1);
    const std::string filename = separator == std::string::npos
        ? original : original.substr(separator + 1);

    const char* extensions[] = {".sqlite.gz", ".sqlite.zlib", ".sqlite"};
    std::string extension;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        if (HasSuffix(filename, extensions[i])) {
            extension = extensions[i];
            break;
        }
    }
    if (extension.empty()) return std::string();

    const std::string stem = filename.substr(0, filename.size() - extension.size());
    const std::string prefix = "BiblioLED_auto_";
    if (stem.compare(0, prefix.size(), prefix) != 0) return std::string();

    const std::time_t now = std::time(NULL);
    const std::tm* local = std::localtime(&now);
    char timestamp[32] = {0};
    if (!local || std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", local) == 0) {
        return std::string();
    }

    std::ostringstream name;
    name << "BiblioLED_invalid_auto_" << stem.substr(prefix.size())
         << "_" << timestamp;
    if (suffix > 0) name << "_" << suffix;
    name << extension;
    return directory + name.str();
}

std::string SqliteError(sqlite3* db, const std::string& operation)
{
    std::string message = operation;
    message += ": ";
    message += db ? sqlite3_errmsg(db) : "SQLite database is not open";
    return message;
}

std::string MakeTemporaryDatabasePath(const std::string& base,
                                     const std::string& purpose)
{
    return MakeTemporaryBackupPath(base, purpose);
}

std::string MakeRecoveryPreservedPath(const std::string& database_path, int suffix)
{
    const std::time_t now = std::time(NULL);
    const std::tm* local = std::localtime(&now);
    char timestamp[32] = {0};
    if (!local ||
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", local) == 0) {
        return std::string();
    }

    std::ostringstream path;
    path << database_path << ".recovery_preserved_" << timestamp;
    if (suffix > 0) path << "_" << suffix;
    return path.str();
}


} // namespace

LoanStore::LoanStore()
    : db_(NULL),
      session_backup_done_(false),
      schema_version_(-1),
      migration_required_(false),
      external_audit_log_enabled_(false),
      external_audit_log_healthy_(true),
      backup_compression_format_("gzip"),
      backup_compression_level_(6)
{
}

LoanStore::~LoanStore()
{
    Close();
}

void LoanStore::SetError(std::string* error, const std::string& message) const
{
    if (error) *error = message;
}

bool LoanStore::RollbackTransaction(std::string* error)
{
    if (!db_) return false;

    char* rollback_message = NULL;
    const int rollback_rc = sqlite3_exec(
        db_, "ROLLBACK;", NULL, NULL, &rollback_message);
    if (rollback_rc == SQLITE_OK || sqlite3_get_autocommit(db_) != 0) {
        sqlite3_free(rollback_message);
        return true;
    }

    std::string detail = rollback_message
        ? rollback_message : sqlite3_errmsg(db_);
    sqlite3_free(rollback_message);

    // A failed ROLLBACK must never leave this LoanStore usable with an
    // open transaction. Closing the connection makes SQLite discard any
    // transaction that could not be rolled back explicitly.
    sqlite3* failed_db = db_;
    db_ = NULL;
    int close_rc = sqlite3_close(failed_db);
    if (close_rc != SQLITE_OK) {
        close_rc = sqlite3_close_v2(failed_db);
    }
    Close();

    if (error) {
        if (!error->empty()) *error += " ";
        *error += "SQLite rollback failed: ";
        *error += detail;
        *error += ". The database connection was closed to prevent further use.";
        if (close_rc != SQLITE_OK) {
            *error += " SQLite could not confirm connection closure.";
        }
    }
    return false;
}

bool LoanStore::Open(const std::string& database_path,
                     const std::string& backup_directory,
                     std::string* error)
{
    Close();

    database_path_ = database_path;
    backup_directory_ = backup_directory;
    session_backup_done_ = false;
    last_session_backup_path_.clear();
    schema_version_ = -1;
    migration_required_ = false;

    const bool existed = FileExists(database_path);
    const int flags = SQLITE_OPEN_READWRITE | (existed ? 0 : SQLITE_OPEN_CREATE);

    int rc = sqlite3_open_v2(database_path.c_str(), &db_, flags, NULL);
    if (rc != SQLITE_OK) {
        SetError(error, SqliteError(db_, "Unable to open database"));
        Close();
        return false;
    }

    sqlite3_busy_timeout(db_, 3000);

    if (!QuickCheck(error)) {
        Close();
        return false;
    }

    if (!existed) {
        if (!CreateInitialSchema(error)) {
            Close();
            return false;
        }
        schema_version_ = 1;
        migration_required_ = false;
        return true;
    }

    if (!ValidateExistingSchema(error)) {
        Close();
        return false;
    }

    if (!ReadUserVersion(db_, &schema_version_, error)) {
        Close();
        return false;
    }

    if (schema_version_ < 0 || schema_version_ > 1) {
        SetError(error,
            "The database schema version is not supported by this application. "
            "The database has not been modified.");
        Close();
        return false;
    }

    bool has_audit_table = false;
    if (!HasAuditTable(db_, &has_audit_table, error)) {
        Close();
        return false;
    }

    if (schema_version_ == 0) {
        if (has_audit_table) {
            SetError(error,
                "The database reports legacy schema version 0 but already contains "
                "an audit table. Its structure is ambiguous; no migration was attempted.");
            Close();
            return false;
        }
        migration_required_ = true;
        return true;
    }

    if (!has_audit_table || !HasAuditSchema(db_)) {
        SetError(error,
            "The database reports schema version 1 but its audit table is missing "
            "or incomplete. The database has not been modified.");
        Close();
        return false;
    }

    migration_required_ = false;
    return true;
}

void LoanStore::Close()
{
    if (db_) {
        // sqlite3_close() returns SQLITE_BUSY if a statement is still active.
        // Do not lose the connection pointer in that case: close_v2 marks
        // it for deferred closure after outstanding statements are finalised.
        const int close_rc = sqlite3_close(db_);
        if (close_rc != SQLITE_OK) {
            sqlite3_close_v2(db_);
        }
        db_ = NULL;
    }
    database_path_.clear();
    backup_directory_.clear();
    last_session_backup_path_.clear();
    session_backup_done_ = false;
    schema_version_ = -1;
    migration_required_ = false;
    external_audit_log_enabled_ = false;
    external_audit_log_healthy_ = true;
}

bool LoanStore::IsOpen() const
{
    return db_ != NULL;
}

void LoanStore::SetBackupCompression(const std::string& format, int level)
{
    if ((format == "gzip" || format == "zlib") && level >= 0 && level <= 9) {
        backup_compression_format_ = format;
        backup_compression_level_ = level;
    } else {
        backup_compression_format_ = "gzip";
        backup_compression_level_ = 6;
    }
}

int LoanStore::SchemaVersion() const
{
    return schema_version_;
}

bool LoanStore::MigrationRequired() const
{
    return migration_required_;
}

void LoanStore::SetExternalAuditLogStatus(bool enabled, bool healthy)
{
    external_audit_log_enabled_ = enabled;
    external_audit_log_healthy_ = healthy;
}

bool LoanStore::ExternalAuditLogBlocked() const
{
    return external_audit_log_enabled_ && !external_audit_log_healthy_;
}

bool LoanStore::HasSessionBackup() const
{
    return session_backup_done_;
}

const std::string& LoanStore::LastSessionBackupPath() const
{
    return last_session_backup_path_;
}

std::string LoanStore::MakeBackupPath(const std::string& category) const
{
    const std::time_t now = std::time(NULL);
    const std::tm* local = std::localtime(&now);
    if (!local) return std::string();

    char timestamp[32] = {0};
    char date[16] = {0};
    if (std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", local) == 0 ||
        std::strftime(date, sizeof(date), "%Y%m%d", local) == 0) {
        return std::string();
    }

    const std::string extension =
        BackupCompressionExtension(CompressionFormatFromName(backup_compression_format_));

    for (int suffix = 0; suffix < 1000; ++suffix) {
        std::ostringstream filename;
        filename << "BiblioLED_" << category << "_";
        if (category == "auto") {
            filename << date;
            if (suffix > 0) {
                filename << "_" << timestamp;
                if (suffix > 1) filename << "_" << suffix;
            }
        } else {
            filename << timestamp;
            if (suffix > 0) filename << "_" << suffix;
        }
        filename << extension;

        const std::string candidate = JoinPath(backup_directory_, filename.str());
        if (!FileExists(candidate)) return candidate;
    }
    return std::string();
}

bool LoanStore::CreateAndValidateBackup(const std::string& path, std::string* error)
{
    const std::string raw_path = MakeTemporaryBackupPath(path, "sqlite_snapshot");
    const std::string compressed_path = MakeTemporaryBackupPath(path, "compressed");
    const std::string verify_path = MakeTemporaryBackupPath(path, "verify_sqlite");
    if (raw_path.empty() || compressed_path.empty() || verify_path.empty()) {
        SetError(error, "Could not reserve temporary files for the backup operation.");
        return false;
    }

    sqlite3* backup_db = NULL;
    int rc = sqlite3_open_v2(
        raw_path.c_str(), &backup_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE, NULL);
    if (rc != SQLITE_OK) {
        SetError(error, SqliteError(backup_db, "Could not create the temporary SQLite snapshot"));
        if (backup_db) sqlite3_close(backup_db);
        RemoveTemporaryDatabaseFiles(raw_path);
        return false;
    }

    sqlite3_busy_timeout(backup_db, 3000);
    std::string copy_error;
    const bool copied = CopyDatabase(db_, backup_db, &copy_error);
    const int close_rc = sqlite3_close(backup_db);

    if (!copied || close_rc != SQLITE_OK) {
        std::string message = "SQLite backup failed";
        if (!copy_error.empty()) message += ": " + copy_error;
        if (close_rc != SQLITE_OK) message += ": could not close the snapshot file";
        SetError(error, message);
        RemoveTemporaryDatabaseFiles(raw_path);
        return false;
    }

    std::string validation_error;
    if (!ValidateDatabaseFile(raw_path, &validation_error)) {
        SetError(error, "The SQLite snapshot failed integrity validation: " + validation_error);
        RemoveTemporaryDatabaseFiles(raw_path);
        return false;
    }

    const BackupCompressionFormat format =
        CompressionFormatFromName(backup_compression_format_);
    if (!CompressBackupFile(raw_path, compressed_path, format,
                            backup_compression_level_, error)) {
        RemoveTemporaryDatabaseFiles(raw_path);
        return false;
    }

    if (!DecompressBackupFile(compressed_path, verify_path, format, error) ||
        !ValidateDatabaseFile(verify_path, &validation_error)) {
        if (error && error->empty()) {
            *error = "The compressed backup failed its SQLite round-trip integrity check: " +
                     validation_error;
        }
        RemoveTemporaryDatabaseFiles(raw_path);
        RemoveBackupFile(compressed_path);
        RemoveTemporaryDatabaseFiles(verify_path);
        return false;
    }

    if (!MoveBackupFile(compressed_path, path)) {
        SetError(error, "The verified compressed backup could not be moved to its final name.");
        RemoveTemporaryDatabaseFiles(raw_path);
        RemoveBackupFile(compressed_path);
        RemoveTemporaryDatabaseFiles(verify_path);
        return false;
    }

    RemoveTemporaryDatabaseFiles(raw_path);
    RemoveTemporaryDatabaseFiles(verify_path);
    return true;
}

bool LoanStore::EnsureAutomaticBackup(std::string* error)
{
    // Do not short-circuit on a session flag: the application may remain open
    // across midnight, and the next loan mutation must create the next day's
    // snapshot. The dated candidate check below decides whether one exists.
    if (backup_directory_.empty()) {
        SetError(error, "The backup directory is not configured. The database was not changed.");
        return false;
    }

    const std::time_t now = std::time(NULL);
    const std::tm* local = std::localtime(&now);
    if (!local) {
        SetError(error, "Could not determine the local date for the automatic backup.");
        return false;
    }
    char date[16] = {0};
    if (std::strftime(date, sizeof(date), "%Y%m%d", local) == 0) {
        SetError(error, "Could not format the local date for the automatic backup.");
        return false;
    }

    const std::string prefix = JoinPath(backup_directory_,
        std::string("BiblioLED_auto_") + date);
    const std::string candidates[] = {
        prefix + ".sqlite.gz",
        prefix + ".sqlite.zlib",
        prefix + ".sqlite"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!FileExists(candidates[i])) continue;
        std::string decoded_path;
        std::string temporary_path;
        std::string validation_error;
        if (DecodeBackupFile(candidates[i], &decoded_path, &temporary_path,
                             &validation_error) &&
            ValidateDatabaseFile(decoded_path, &validation_error)) {
            if (!temporary_path.empty()) RemoveTemporaryDatabaseFiles(temporary_path);
            last_session_backup_path_ = candidates[i];
            session_backup_done_ = true;
            return true;
        }
        if (!temporary_path.empty()) RemoveTemporaryDatabaseFiles(temporary_path);

        // Keep a corrupt candidate and any SQLite sidecars together for
        // diagnosis, but move the group out of the automatic-backup namespace.
        std::string preservation_error;
        if (!PreserveInvalidBackupGroup(candidates[i], &preservation_error)) {
            SetError(error,
                preservation_error + " No loan data was changed.");
            return false;
        }
    }

    const std::string path = MakeBackupPath("auto");
    if (path.empty()) {
        SetError(error, "Could not choose a unique name for the daily automatic backup. The database was not changed.");
        return false;
    }

    if (!CreateAndValidateBackup(path, error)) {
        if (error && !error->empty()) *error += ". The database was not changed.";
        return false;
    }

    last_session_backup_path_ = path;
    session_backup_done_ = true;

    // Retention is deliberately best-effort: a cleanup failure must not make
    // an already-created, validated safety snapshot unusable.
    std::string retention_error;
    PruneAutomaticBackups(backup_directory_, &retention_error);
    return true;
}

bool LoanStore::CreateManualBackup(std::string* backup_path, std::string* error)
{
    if (!db_) {
        SetError(error, "Database is not open.");
        return false;
    }

    const std::string path = MakeBackupPath("manual");
    if (path.empty()) {
        SetError(error, "Could not choose a unique name for the manual backup.");
        return false;
    }
    if (!CreateAndValidateBackup(path, error)) return false;

    if (backup_path) *backup_path = path;
    return true;
}

bool LoanStore::RestoreFromBackup(const std::string& backup_path,
                                  std::string* current_backup_path,
                                  std::string* error)
{
    if (!db_ || backup_path.empty()) {
        SetError(error, "An open database and a backup path are required.");
        return false;
    }
    if (ExternalAuditLogBlocked()) {
        SetError(error, "The external audit log is unavailable. Database restore is blocked until logging is working again.");
        return false;
    }
    if (backup_path == database_path_) {
        SetError(error, "The selected backup is the currently open database.");
        return false;
    }

    std::string validation_error;
    std::string decoded_backup_path;
    std::string decoded_temporary_path;
    if (!DecodeBackupFile(backup_path, &decoded_backup_path,
                          &decoded_temporary_path, &validation_error)) {
        SetError(error, "The selected backup could not be decompressed: " + validation_error);
        return false;
    }
    ScopedBackupFile decoded_cleanup;
    decoded_cleanup.path = decoded_temporary_path;

    if (!ValidateDatabaseFile(decoded_backup_path, &validation_error)) {
        SetError(error, "The selected backup is invalid: " + validation_error);
        return false;
    }

    sqlite3* source_db = NULL;
    int rc = sqlite3_open_v2(decoded_backup_path.c_str(), &source_db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        SetError(error, SqliteError(source_db, "Could not open the selected backup"));
        if (source_db) sqlite3_close(source_db);
        return false;
    }

    if (!HasExpectedSchema(source_db)) {
        SetError(error, "The selected backup does not have a compatible loans schema. The current database was not changed.");
        sqlite3_close(source_db);
        return false;
    }

    int source_version = -1;
    bool source_has_audit = false;
    if (!ReadUserVersion(source_db, &source_version, error) ||
        source_version < 0 || source_version > 1 ||
        !HasAuditTable(source_db, &source_has_audit, error) ||
        (source_version == 0 && source_has_audit) ||
        (source_version == 1 && (!source_has_audit || !HasAuditSchema(source_db)))) {
        SetError(error, "The selected backup has an unsupported or inconsistent schema. The current database was not changed.");
        sqlite3_close(source_db);
        return false;
    }

    const std::string safety_backup = MakeBackupPath("safety_restore");
    if (safety_backup.empty()) {
        SetError(error, "Could not choose a name for the pre-restore backup. The current database was not changed.");
        sqlite3_close(source_db);
        return false;
    }
    if (!CreateAndValidateBackup(safety_backup, error)) {
        sqlite3_close(source_db);
        if (error && !error->empty()) *error += ". The current database was not changed.";
        return false;
    }
    if (current_backup_path) *current_backup_path = safety_backup;

    std::string restore_error;
    bool restored = CopyDatabase(source_db, db_, &restore_error);
    const int source_close_rc = sqlite3_close(source_db);
    if (source_close_rc != SQLITE_OK) {
        restored = false;
        restore_error = "Could not close the selected backup after copying.";
    }

    int restored_version = -1;
    bool restored_has_audit = false;
    if (restored) {
        restored = QuickCheck(&restore_error) &&
                   ValidateExistingSchema(&restore_error) &&
                   ReadUserVersion(db_, &restored_version, &restore_error) &&
                   HasAuditTable(db_, &restored_has_audit, &restore_error);
    }
    if (restored && restored_version == 1 &&
        (!restored_has_audit || !HasAuditSchema(db_))) {
        restored = false;
        restore_error = "Restored schema version 1 is missing its audit table.";
    }
    if (restored && (restored_version < 0 || restored_version > 1 ||
        (restored_version == 0 && restored_has_audit))) {
        restored = false;
        restore_error = "Restored database has an unsupported or inconsistent schema version.";
    }

    if (!restored) {
        sqlite3* fallback_db = NULL;
        std::string fallback_source_path;
        std::string fallback_temporary_path;
        std::string rollback_error;
        bool fallback_decoded = DecodeBackupFile(
            safety_backup, &fallback_source_path, &fallback_temporary_path,
            &rollback_error);
        ScopedBackupFile fallback_cleanup;
        fallback_cleanup.path = fallback_temporary_path;
        const int fallback_open = fallback_decoded
            ? sqlite3_open_v2(fallback_source_path.c_str(), &fallback_db, SQLITE_OPEN_READONLY, NULL)
            : SQLITE_ERROR;
        bool rolled_back = false;
        if (fallback_open == SQLITE_OK && fallback_db) {
            int fallback_version = -1;
            bool fallback_has_audit = false;
            rolled_back = ReadUserVersion(fallback_db, &fallback_version, &rollback_error) &&
                          HasAuditTable(fallback_db, &fallback_has_audit, &rollback_error) &&
                          fallback_version >= 0 && fallback_version <= 1 &&
                          ((fallback_version == 0 && !fallback_has_audit) ||
                           (fallback_version == 1 && fallback_has_audit && HasAuditSchema(fallback_db))) &&
                          CopyDatabase(fallback_db, db_, &rollback_error);
            sqlite3_close(fallback_db);
            if (rolled_back) {
                rolled_back = QuickCheck(&rollback_error) &&
                              ValidateExistingSchema(&rollback_error);
            }
            if (rolled_back) {
                schema_version_ = fallback_version;
                migration_required_ = (fallback_version == 0);
            }
            if (!rolled_back && !rollback_error.empty()) {
                restore_error += " Rollback attempt also failed: " + rollback_error;
            }
        } else {
            if (fallback_db) sqlite3_close(fallback_db);
            restore_error += " Could not open or decompress the pre-restore backup for rollback.";
            if (!rollback_error.empty()) restore_error += " " + rollback_error;
        }

        if (rolled_back) {
            SetError(error, "Restore was not completed. The original database was restored from the safety copy. " + restore_error);
        } else {
            schema_version_ = -1;
            migration_required_ = true;
            SetError(error, "Restore failed and automatic rollback could not be verified. Preserve the safety backup at: " +
                            safety_backup + ". " + restore_error);
        }
        return false;
    }

    schema_version_ = restored_version;
    migration_required_ = (restored_version == 0);
    return true;
}

bool LoanStore::RecoverFromBackup(const std::string& database_path,
                                  const std::string& backup_path,
                                  std::string* preserved_database_path,
                                  std::string* error)
{
    if (preserved_database_path) preserved_database_path->clear();
    if (error) error->clear();

    if (db_) {
        SetError(error, "Close the current database before attempting recovery.");
        return false;
    }
    if (database_path.empty() || backup_path.empty()) {
        SetError(error, "A database path and a backup path are required for recovery.");
        return false;
    }
    if (database_path == backup_path) {
        SetError(error, "The selected backup is the expected database file.");
        return false;
    }

    std::string decoded_backup_path;
    std::string decoded_temporary_path;
    std::string validation_error;
    if (!DecodeBackupFile(backup_path, &decoded_backup_path,
                          &decoded_temporary_path, &validation_error)) {
        SetError(error, "The selected backup could not be decompressed: " + validation_error);
        return false;
    }
    ScopedBackupFile decoded_cleanup;
    decoded_cleanup.path = decoded_temporary_path;

    if (!ValidateDatabaseFile(decoded_backup_path, &validation_error)) {
        SetError(error, "The selected backup is invalid: " + validation_error);
        return false;
    }

    sqlite3* source_db = NULL;
    int rc = sqlite3_open_v2(
        decoded_backup_path.c_str(), &source_db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        SetError(error, SqliteError(source_db, "Could not open the selected recovery backup"));
        if (source_db) sqlite3_close(source_db);
        return false;
    }

    const std::string staging_path =
        MakeTemporaryDatabasePath(database_path, "recovery_staging");
    if (staging_path.empty()) {
        sqlite3_close(source_db);
        SetError(error, "Could not reserve a temporary database file for recovery.");
        return false;
    }

    sqlite3* staging_db = NULL;
    rc = sqlite3_open_v2(
        staging_path.c_str(), &staging_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE, NULL);
    if (rc != SQLITE_OK) {
        SetError(error, SqliteError(staging_db, "Could not create the recovery staging database"));
        if (staging_db) sqlite3_close(staging_db);
        sqlite3_close(source_db);
        RemoveTemporaryDatabaseFiles(staging_path);
        return false;
    }

    std::string copy_error;
    const bool copied = CopyDatabase(source_db, staging_db, &copy_error);
    const int staging_close_rc = sqlite3_close(staging_db);
    const int source_close_rc = sqlite3_close(source_db);
    if (!copied || staging_close_rc != SQLITE_OK || source_close_rc != SQLITE_OK) {
        std::string message = "Could not prepare the recovery database";
        if (!copy_error.empty()) message += ": " + copy_error;
        if (staging_close_rc != SQLITE_OK) message += "; staging database could not be closed";
        if (source_close_rc != SQLITE_OK) message += "; recovery backup could not be closed";
        SetError(error, message);
        RemoveTemporaryDatabaseFiles(staging_path);
        return false;
    }

    if (!ValidateDatabaseFile(staging_path, &validation_error)) {
        SetError(error, "The recovery staging database failed validation: " + validation_error);
        RemoveTemporaryDatabaseFiles(staging_path);
        return false;
    }

    // Keep the main database and all SQLite sidecars together. A stale WAL or
    // rollback journal must never be allowed to attach itself to the recovered file.
    const std::string sidecar_suffixes[] = {"", "-journal", "-wal", "-shm"};
    std::string original_paths[4];
    std::string preserved_paths[4];
    bool original_exists[4] = {false, false, false, false};
    bool any_original = false;
    for (size_t i = 0; i < 4; ++i) {
        original_paths[i] = database_path + sidecar_suffixes[i];
        original_exists[i] = FileExists(original_paths[i]);
        any_original = any_original || original_exists[i];
    }

    std::string preserved_base;
    bool names_available = false;
    for (int suffix = 0; suffix < 1000; ++suffix) {
        preserved_base = MakeRecoveryPreservedPath(database_path, suffix);
        if (preserved_base.empty()) break;
        names_available = true;
        for (size_t i = 0; i < 4; ++i) {
            preserved_paths[i] = preserved_base + sidecar_suffixes[i];
            if (original_exists[i] && FileExists(preserved_paths[i])) {
                names_available = false;
                break;
            }
        }
        if (names_available) break;
    }
    if (!names_available) {
        SetError(error, "Could not choose unused names to preserve the existing database files.");
        RemoveTemporaryDatabaseFiles(staging_path);
        return false;
    }

    std::vector<std::string> moved_sources;
    std::vector<std::string> moved_destinations;
    for (size_t i = 0; i < 4; ++i) {
        if (!original_exists[i]) continue;
        if (!MoveBackupFile(original_paths[i], preserved_paths[i])) {
            bool rollback_ok = true;
            for (size_t moved = moved_sources.size(); moved > 0; --moved) {
                if (!MoveBackupFile(moved_destinations[moved - 1],
                                    moved_sources[moved - 1])) {
                    rollback_ok = false;
                }
            }
            std::string message =
                "Could not preserve the existing database or its SQLite sidecars. "
                "The recovery database was not installed.";
            if (!rollback_ok) {
                message += " Some original files remain in the recovery-preserved location: ";
                message += preserved_base;
            }
            SetError(error, message);
            RemoveTemporaryDatabaseFiles(staging_path);
            return false;
        }
        moved_sources.push_back(original_paths[i]);
        moved_destinations.push_back(preserved_paths[i]);
    }

    if (!MoveBackupFile(staging_path, database_path)) {
        bool rollback_ok = true;
        for (size_t moved = moved_sources.size(); moved > 0; --moved) {
            if (!MoveBackupFile(moved_destinations[moved - 1],
                                moved_sources[moved - 1])) {
                rollback_ok = false;
            }
        }
        std::string message =
            "The validated recovery database could not be installed at the expected path.";
        if (!rollback_ok) {
            message += " Some original files remain preserved at: ";
            message += preserved_base;
        }
        SetError(error, message);
        RemoveTemporaryDatabaseFiles(staging_path);
        return false;
    }

    if (!ValidateDatabaseFile(database_path, &validation_error)) {
        const std::string failed_path = preserved_base + ".recovery_failed";
        const bool failed_file_preserved =
            !FileExists(failed_path) && MoveBackupFile(database_path, failed_path);
        bool rollback_ok = failed_file_preserved;
        for (size_t moved = moved_sources.size(); moved > 0; --moved) {
            if (!MoveBackupFile(moved_destinations[moved - 1],
                                moved_sources[moved - 1])) {
                rollback_ok = false;
            }
        }
        std::string message =
            "The installed recovery database failed its final validation: " +
            validation_error;
        if (rollback_ok) {
            message += " The original database files were restored where present.";
        } else {
            message += " Automatic rollback was incomplete. Preserve the recovery files at: ";
            message += preserved_base;
            if (!failed_file_preserved) {
                message += ". The unverified restored file may still be at the expected database path.";
            }
        }
        SetError(error, message);
        return false;
    }

    if (preserved_database_path && any_original) {
        *preserved_database_path = preserved_base;
    }
    return true;
}

bool LoanStore::CreateInitialSchema(std::string* error)
{
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error)) return false;

    const char* loans_sql =
        "CREATE TABLE loans ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " title TEXT NOT NULL,"
        " loan_date TEXT NOT NULL,"
        " type TEXT NOT NULL"
        ");";

    bool ok = ExecuteSql(db_, loans_sql, error);
    if (ok) ok = CreateAuditSchema(db_, error);
    if (ok) ok = ExecuteSql(db_, "PRAGMA user_version = 1;", error);
    if (ok) ok = QuickCheck(error);

    if (!ok) {
        RollbackTransaction(error);
        return false;
    }

    if (!ExecuteSql(db_, "COMMIT;", error)) {
        RollbackTransaction(error);
        return false;
    }
    return true;
}

bool LoanStore::MigrateLegacySchema(std::string* backup_path, std::string* error)
{
    if (!db_) {
        SetError(error, "Database is not open.");
        return false;
    }
    if (!migration_required_ || schema_version_ != 0) {
        SetError(error, "The database does not require the legacy schema migration.");
        return false;
    }
    if (ExternalAuditLogBlocked()) {
        SetError(error, "The external audit log is unavailable. Database migration is blocked until logging is working again.");
        return false;
    }
    if (backup_directory_.empty()) {
        SetError(error, "The backup directory is not configured. The database was not changed.");
        return false;
    }

    const std::string safety_backup = MakeBackupPath("safety_migration");
    if (safety_backup.empty()) {
        SetError(error, "Could not choose a name for the migration backup. The database was not changed.");
        return false;
    }
    if (!CreateAndValidateBackup(safety_backup, error)) {
        if (error && !error->empty()) *error += ". The database was not changed.";
        return false;
    }
    if (backup_path) *backup_path = safety_backup;
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error)) {
        if (error && !error->empty()) *error = "Could not start the schema migration: " + *error;
        return false;
    }

    int current_version = -1;
    bool has_audit_table = false;
    bool ok = ReadUserVersion(db_, &current_version, error);
    if (ok && current_version != 0) {
        SetError(error, "The database schema changed after it was inspected. Migration cancelled.");
        ok = false;
    }
    if (ok) ok = HasAuditTable(db_, &has_audit_table, error);
    if (ok && has_audit_table) {
        SetError(error, "An audit table already exists in schema version 0. Migration cancelled.");
        ok = false;
    }
    if (ok) ok = CreateAuditSchema(db_, error);
    if (ok) {
        ok = InsertAuditEvent(db_, "schema_migration", 0, NULL, NULL,
                              "schema_version: 0 -> 1", error);
    }
    if (ok) ok = ExecuteSql(db_, "PRAGMA user_version = 1;", error);
    if (ok && (!HasExpectedSchema(db_) || !HasAuditSchema(db_))) {
        SetError(error, "The migrated schema did not pass structural validation.");
        ok = false;
    }
    if (ok) ok = QuickCheck(error);

    if (!ok) {
        const bool rollback_ok = RollbackTransaction(error);
        if (error && !error->empty()) {
            const std::string prefix = rollback_ok
                ? "The schema migration was rolled back. The original database remains unchanged: "
                : "The schema migration failed and its database connection was closed after rollback could not be confirmed: ";
            *error = prefix + *error;
        }
        return false;
    }

    if (!ExecuteSql(db_, "COMMIT;", error)) {
        const bool rollback_ok = RollbackTransaction(error);
        if (error && !error->empty()) {
            const std::string prefix = rollback_ok
                ? "Could not commit the schema migration; it was rolled back: "
                : "Could not commit the schema migration; the database connection was closed after rollback could not be confirmed: ";
            *error = prefix + *error;
        }
        return false;
    }

    schema_version_ = 1;
    migration_required_ = false;
    return true;
}

bool LoanStore::ValidateExistingSchema(std::string* error)
{
    if (!HasExpectedSchema(db_)) {
        SetError(error,
            "The existing database does not contain the expected loans schema. "
            "It has not been modified.");
        return false;
    }
    return true;
}

bool LoanStore::QuickCheck(std::string* error)
{
    if (!db_) {
        SetError(error, "Database is not open.");
        return false;
    }

    sqlite3_stmt* statement = NULL;
    int rc = sqlite3_prepare_v2(db_, "PRAGMA quick_check;", -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        SetError(error, SqliteError(db_, "SQLite quick_check failed"));
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    bool valid = true;
    bool saw_result = false;
    std::string check_error;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        saw_result = true;
        const unsigned char* result = sqlite3_column_text(statement, 0);
        if (!result || sqlite3_stricmp(
                reinterpret_cast<const char*>(result), "ok") != 0) {
            valid = false;
            if (check_error.empty()) {
                check_error = result
                    ? reinterpret_cast<const char*>(result)
                    : "SQLite quick_check returned an empty result.";
            }
        }
    }
    if (rc != SQLITE_DONE && check_error.empty()) {
        check_error = sqlite3_errmsg(db_);
    }

    const int finalize_rc = sqlite3_finalize(statement);
    if (finalize_rc != SQLITE_OK && check_error.empty()) {
        check_error = sqlite3_errmsg(db_);
    }
    if (rc != SQLITE_DONE || finalize_rc != SQLITE_OK ||
        !saw_result || !valid) {
        std::string message = "Database integrity check failed";
        if (!check_error.empty()) message += ": " + check_error;
        SetError(error, message);
        return false;
    }
    return true;
}

bool LoanStore::GetAuditEvents(std::vector<AuditEvent>* events, std::string* error)
{
    if (!db_ || !events) {
        SetError(error, "Invalid database or output argument.");
        return false;
    }
    if (migration_required_) {
        SetError(error, "The legacy database must be migrated before audit events can be read.");
        return false;
    }

    events->clear();
    sqlite3_stmt* statement = NULL;
    const char* sql =
        "SELECT id, occurred_at, action, loan_id, "
        "before_title, before_loan_date, before_type, "
        "after_title, after_loan_date, after_type, details "
        "FROM audit_events ORDER BY id;";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        SetError(error, SqliteError(db_, "Unable to prepare audit-event query"));
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        AuditEvent event;
        event.id = sqlite3_column_int64(statement, 0);
        const unsigned char* occurred_at = sqlite3_column_text(statement, 1);
        const unsigned char* action = sqlite3_column_text(statement, 2);
        event.occurred_at = occurred_at ? reinterpret_cast<const char*>(occurred_at) : "";
        event.action = action ? reinterpret_cast<const char*>(action) : "";
        if (sqlite3_column_type(statement, 3) != SQLITE_NULL) {
            event.loan_id = sqlite3_column_int(statement, 3);
        }

        if (sqlite3_column_type(statement, 4) != SQLITE_NULL) {
            event.has_before = true;
            event.before.id = event.loan_id;
            const unsigned char* title = sqlite3_column_text(statement, 4);
            const unsigned char* loan_date = sqlite3_column_text(statement, 5);
            const unsigned char* type = sqlite3_column_text(statement, 6);
            event.before.title = title ? reinterpret_cast<const char*>(title) : "";
            event.before.loan_date = loan_date ? reinterpret_cast<const char*>(loan_date) : "";
            event.before.type = type ? reinterpret_cast<const char*>(type) : "";
        }

        if (sqlite3_column_type(statement, 7) != SQLITE_NULL) {
            event.has_after = true;
            event.after.id = event.loan_id;
            const unsigned char* title = sqlite3_column_text(statement, 7);
            const unsigned char* loan_date = sqlite3_column_text(statement, 8);
            const unsigned char* type = sqlite3_column_text(statement, 9);
            event.after.title = title ? reinterpret_cast<const char*>(title) : "";
            event.after.loan_date = loan_date ? reinterpret_cast<const char*>(loan_date) : "";
            event.after.type = type ? reinterpret_cast<const char*>(type) : "";
        }

        const unsigned char* details = sqlite3_column_text(statement, 10);
        event.details = details ? reinterpret_cast<const char*>(details) : "";
        events->push_back(event);
    }

    if (rc != SQLITE_DONE) {
        SetError(error, SqliteError(db_, "Unable to read audit events"));
        sqlite3_finalize(statement);
        return false;
    }
    return FinalizeSqliteStatement(
        db_, statement, "Unable to finalise audit-event query", error);
}

bool LoanStore::RecordAuditEventInternal(const std::string& action,
                                        const std::string& details,
                                        bool allow_external_log_block,
                                        std::string* error)
{
    if (!db_ || action.empty()) {
        SetError(error, "An open database and a non-empty audit action are required.");
        return false;
    }
    if (migration_required_) {
        SetError(error, "The legacy database must be migrated before audit events can be recorded.");
        return false;
    }
    if (!allow_external_log_block && ExternalAuditLogBlocked()) {
        SetError(error, "The external audit log is unavailable. Database changes are blocked until logging is working again.");
        return false;
    }

    // Audit-only writes do not change loan data and must not trigger backups.
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error)) return false;

    if (!InsertAuditEvent(db_, action.c_str(), 0, NULL, NULL, details, error)) {
        RollbackTransaction(error);
        return false;
    }
    if (!ExecuteSql(db_, "COMMIT;", error)) {
        RollbackTransaction(error);
        return false;
    }
    return true;
}

bool LoanStore::RecordAuditEvent(const std::string& action,
                                 const std::string& details,
                                 std::string* error)
{
    return RecordAuditEventInternal(action, details, false, error);
}

bool LoanStore::RecordDiagnosticEvent(const std::string& severity,
                                      const std::string& details,
                                      std::string* error)
{
    if (severity != "warning" && severity != "error" &&
        severity != "failure") {
        SetError(error, "Diagnostic severity must be warning, error, or failure.");
        return false;
    }

    return RecordAuditEventInternal(
        "application_" + severity, details, true, error);
}

bool LoanStore::GetAll(std::vector<LoanRecord>* records, std::string* error)
{
    if (!db_ || !records) {
        SetError(error, "Invalid database or output argument.");
        return false;
    }

    records->clear();
    sqlite3_stmt* statement = NULL;
    const char* sql =
        "SELECT id, title, loan_date, type "
        "FROM loans ORDER BY loan_date DESC, id DESC;";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        SetError(error, SqliteError(db_, "Unable to prepare loan query"));
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        LoanRecord record;
        record.id = sqlite3_column_int(statement, 0);

        const unsigned char* title = sqlite3_column_text(statement, 1);
        const unsigned char* loan_date = sqlite3_column_text(statement, 2);
        const unsigned char* type = sqlite3_column_text(statement, 3);

        record.title = title ? reinterpret_cast<const char*>(title) : "";
        record.loan_date = loan_date ? reinterpret_cast<const char*>(loan_date) : "";
        record.type = type ? reinterpret_cast<const char*>(type) : "";
        records->push_back(record);
    }

    if (rc != SQLITE_DONE) {
        SetError(error, SqliteError(db_, "Unable to read loans"));
        sqlite3_finalize(statement);
        return false;
    }
    return FinalizeSqliteStatement(
        db_, statement, "Unable to finalise loan query", error);
}

bool LoanStore::GetById(int id, LoanRecord* record, std::string* error)
{
    if (!db_ || !record) {
        SetError(error, "Invalid database or output argument.");
        return false;
    }

    sqlite3_stmt* statement = NULL;
    const char* sql =
        "SELECT id, title, loan_date, type FROM loans WHERE id = ?1;";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        SetError(error, SqliteError(db_, "Unable to prepare loan lookup"));
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    rc = sqlite3_bind_int(statement, 1, id);
    if (!CheckSqliteResult(db_, rc, SQLITE_OK,
                           "Unable to bind loan identifier", error)) {
        sqlite3_finalize(statement);
        return false;
    }

    rc = sqlite3_step(statement);
    const bool found = rc == SQLITE_ROW;
    if (found) {
        record->id = sqlite3_column_int(statement, 0);
        const unsigned char* title = sqlite3_column_text(statement, 1);
        const unsigned char* loan_date = sqlite3_column_text(statement, 2);
        const unsigned char* type = sqlite3_column_text(statement, 3);
        record->title = title ? reinterpret_cast<const char*>(title) : "";
        record->loan_date = loan_date ? reinterpret_cast<const char*>(loan_date) : "";
        record->type = type ? reinterpret_cast<const char*>(type) : "";
        rc = sqlite3_step(statement);
    }

    if (rc != SQLITE_DONE) {
        SetError(error, SqliteError(db_, "Unable to read loan record"));
        sqlite3_finalize(statement);
        return false;
    }
    if (!FinalizeSqliteStatement(
            db_, statement, "Unable to finalise loan lookup", error)) {
        return false;
    }
    if (!found) SetError(error, "Loan record was not found.");
    return found;
}

bool LoanStore::Insert(LoanRecord* record, std::string* error)
{
    if (!db_ || !record || record->title.empty() ||
        record->loan_date.empty() || record->type.empty()) {
        SetError(error, "A valid database, title, date and type are required.");
        return false;
    }
    if (ParseLoanDateTime(record->loan_date) == static_cast<std::time_t>(-1)) {
        SetError(error, "The loan date must use the valid YYYY-MM-DD HH:MM format.");
        return false;
    }
    if (migration_required_) {
        SetError(error, "The legacy database must be migrated before it can be changed.");
        return false;
    }
    if (ExternalAuditLogBlocked()) {
        SetError(error, "The external audit log is unavailable. Database changes are blocked until logging is working again.");
        return false;
    }
    if (!EnsureAutomaticBackup(error)) return false;
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error)) return false;

    sqlite3_stmt* statement = NULL;
    const char* sql =
        "INSERT INTO loans (title, loan_date, type) VALUES (?1, ?2, ?3);";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &statement, NULL);
    bool ok = rc == SQLITE_OK;
    if (!ok) SetError(error, SqliteError(db_, "Unable to prepare loan insertion"));

    if (ok) {
        ok = CheckSqliteResult(db_,
            sqlite3_bind_text(statement, 1, record->title.c_str(), -1, SQLITE_TRANSIENT),
            SQLITE_OK, "Unable to bind loan title", error) &&
             CheckSqliteResult(db_,
            sqlite3_bind_text(statement, 2, record->loan_date.c_str(), -1, SQLITE_TRANSIENT),
            SQLITE_OK, "Unable to bind loan date", error) &&
             CheckSqliteResult(db_,
            sqlite3_bind_text(statement, 3, record->type.c_str(), -1, SQLITE_TRANSIENT),
            SQLITE_OK, "Unable to bind loan type", error);
    }
    if (ok) {
        rc = sqlite3_step(statement);
        ok = CheckSqliteResult(db_, rc, SQLITE_DONE, "Unable to insert loan", error);
    }
    if (statement) {
        if (ok) {
            ok = FinalizeSqliteStatement(
                db_, statement, "Unable to finalise loan insertion", error);
        } else {
            sqlite3_finalize(statement);
        }
    }

    const int new_id = ok
        ? static_cast<int>(sqlite3_last_insert_rowid(db_)) : 0;
    if (ok) ok = InsertAuditEvent(db_, "create", new_id, NULL, record, "", error);

    if (!ok) {
        RollbackTransaction(error);
        return false;
    }
    if (!ExecuteSql(db_, "COMMIT;", error)) {
        RollbackTransaction(error);
        return false;
    }
    record->id = new_id;
    return true;
}

bool LoanStore::Update(const LoanRecord& record, std::string* error)
{
    if (!db_ || record.id <= 0 || record.title.empty() ||
        record.loan_date.empty() || record.type.empty()) {
        SetError(error, "A valid record is required for update.");
        return false;
    }
    if (ParseLoanDateTime(record.loan_date) == static_cast<std::time_t>(-1)) {
        SetError(error, "The loan date must use the valid YYYY-MM-DD HH:MM format.");
        return false;
    }
    if (migration_required_) {
        SetError(error, "The legacy database must be migrated before it can be changed.");
        return false;
    }
    if (ExternalAuditLogBlocked()) {
        SetError(error, "The external audit log is unavailable. Database changes are blocked until logging is working again.");
        return false;
    }

    // Check the target before creating the daily snapshot. A stale UI or an
    // invalid identifier must not create a backup when no loan can be changed.
    LoanRecord existing;
    bool existing_found = false;
    if (!ReadLoanById(db_, record.id, &existing, &existing_found, error)) return false;
    if (!existing_found) {
        SetError(error, "Loan record was not found.");
        return false;
    }

    if (!EnsureAutomaticBackup(error)) return false;
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error)) return false;

    LoanRecord before;
    bool found = false;
    if (!ReadLoanById(db_, record.id, &before, &found, error)) {
        RollbackTransaction(error);
        return false;
    }
    if (!found) {
        SetError(error, "Loan record was not found.");
        RollbackTransaction(error);
        return false;
    }

    sqlite3_stmt* statement = NULL;
    const char* sql =
        "UPDATE loans SET title = ?1, loan_date = ?2, type = ?3 WHERE id = ?4;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &statement, NULL);
    bool ok = rc == SQLITE_OK;
    if (!ok) SetError(error, SqliteError(db_, "Unable to prepare loan update"));

    if (ok) {
        ok = CheckSqliteResult(db_,
            sqlite3_bind_text(statement, 1, record.title.c_str(), -1, SQLITE_TRANSIENT),
            SQLITE_OK, "Unable to bind updated loan title", error) &&
             CheckSqliteResult(db_,
            sqlite3_bind_text(statement, 2, record.loan_date.c_str(), -1, SQLITE_TRANSIENT),
            SQLITE_OK, "Unable to bind updated loan date", error) &&
             CheckSqliteResult(db_,
            sqlite3_bind_text(statement, 3, record.type.c_str(), -1, SQLITE_TRANSIENT),
            SQLITE_OK, "Unable to bind updated loan type", error) &&
             CheckSqliteResult(db_,
            sqlite3_bind_int(statement, 4, record.id),
            SQLITE_OK, "Unable to bind updated loan identifier", error);
    }
    if (ok) {
        rc = sqlite3_step(statement);
        ok = CheckSqliteResult(db_, rc, SQLITE_DONE, "Unable to update loan", error);
        if (ok && sqlite3_changes(db_) == 0) {
            SetError(error, "The requested loan update did not change a record.");
            ok = false;
        }
    }
    if (statement) {
        if (ok) {
            ok = FinalizeSqliteStatement(
                db_, statement, "Unable to finalise loan update", error);
        } else {
            sqlite3_finalize(statement);
        }
    }

    if (ok) ok = InsertAuditEvent(db_, "update", record.id, &before, &record, "", error);
    if (!ok) {
        RollbackTransaction(error);
        return false;
    }
    if (!ExecuteSql(db_, "COMMIT;", error)) {
        RollbackTransaction(error);
        return false;
    }
    return true;
}

bool LoanStore::Remove(int id, std::string* error)
{
    if (!db_ || id <= 0) {
        SetError(error, "A valid record identifier is required.");
        return false;
    }
    if (migration_required_) {
        SetError(error, "The legacy database must be migrated before it can be changed.");
        return false;
    }
    if (ExternalAuditLogBlocked()) {
        SetError(error, "The external audit log is unavailable. Database changes are blocked until logging is working again.");
        return false;
    }

    // As with updates, a missing identifier is not a data mutation and must
    // not trigger a daily snapshot.
    LoanRecord existing;
    bool existing_found = false;
    if (!ReadLoanById(db_, id, &existing, &existing_found, error)) return false;
    if (!existing_found) {
        SetError(error, "Loan record was not found.");
        return false;
    }

    if (!EnsureAutomaticBackup(error)) return false;
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error)) return false;

    LoanRecord before;
    bool found = false;
    if (!ReadLoanById(db_, id, &before, &found, error)) {
        RollbackTransaction(error);
        return false;
    }
    if (!found) {
        SetError(error, "Loan record was not found.");
        RollbackTransaction(error);
        return false;
    }

    sqlite3_stmt* statement = NULL;
    int rc = sqlite3_prepare_v2(
        db_, "DELETE FROM loans WHERE id = ?1;", -1, &statement, NULL);
    bool ok = rc == SQLITE_OK;
    if (!ok) SetError(error, SqliteError(db_, "Unable to prepare loan deletion"));

    if (ok) {
        ok = CheckSqliteResult(db_, sqlite3_bind_int(statement, 1, id),
                               SQLITE_OK, "Unable to bind loan identifier", error);
    }
    if (ok) {
        rc = sqlite3_step(statement);
        ok = CheckSqliteResult(db_, rc, SQLITE_DONE, "Unable to delete loan", error);
        if (ok && sqlite3_changes(db_) == 0) {
            SetError(error, "The requested loan deletion did not remove a record.");
            ok = false;
        }
    }
    if (statement) {
        if (ok) {
            ok = FinalizeSqliteStatement(
                db_, statement, "Unable to finalise loan deletion", error);
        } else {
            sqlite3_finalize(statement);
        }
    }

    if (ok) ok = InsertAuditEvent(db_, "delete", id, &before, NULL, "", error);
    if (!ok) {
        RollbackTransaction(error);
        return false;
    }
    if (!ExecuteSql(db_, "COMMIT;", error)) {
        RollbackTransaction(error);
        return false;
    }
    return true;
}

bool LoanStore::CountSince(const std::string& date_time, int* count, std::string* error)
{
    if (!db_ || !count) {
        SetError(error, "Invalid database or output argument.");
        return false;
    }

    sqlite3_stmt* statement = NULL;
    int rc = sqlite3_prepare_v2(
        db_, "SELECT COUNT(*) FROM loans WHERE loan_date >= ?1;", -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        SetError(error, SqliteError(db_, "Unable to prepare loan count"));
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    if (!CheckSqliteResult(db_,
            sqlite3_bind_text(statement, 1, date_time.c_str(), -1, SQLITE_TRANSIENT),
            SQLITE_OK, "Unable to bind loan-count date", error)) {
        sqlite3_finalize(statement);
        return false;
    }

    rc = sqlite3_step(statement);
    if (rc != SQLITE_ROW) {
        SetError(error, SqliteError(db_, "Unable to count loans"));
        sqlite3_finalize(statement);
        return false;
    }
    const int result_count = sqlite3_column_int(statement, 0);
    rc = sqlite3_step(statement);
    if (rc != SQLITE_DONE) {
        SetError(error, SqliteError(db_, "Unable to complete loan count"));
        sqlite3_finalize(statement);
        return false;
    }
    if (!FinalizeSqliteStatement(
            db_, statement, "Unable to finalise loan count", error)) {
        return false;
    }

    *count = result_count;
    return true;
}

} // namespace biblioled
