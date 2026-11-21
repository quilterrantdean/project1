#include "biblioled/loan_store.h"
#include "biblioled/backup_compression.h"

#include <sqlite3.h>

#include <cerrno>
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
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace biblioled {
class LoanStoreTestAccess {
public:
    static sqlite3* Database(LoanStore& store)
    {
        return store.db_;
    }

    static bool RollbackTransaction(LoanStore& store, std::string* error)
    {
        return store.RollbackTransaction(error);
    }
};
} // namespace biblioled

namespace {

int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

bool Execute(sqlite3* db, const char* sql)
{
    char* message = NULL;
    const int rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        std::cerr << (message ? message : sqlite3_errmsg(db)) << std::endl;
    }
    sqlite3_free(message);
    return rc == SQLITE_OK;
}

int DenyRollbackAuthorizer(void*,
                           int action,
                           const char* first_argument,
                           const char*,
                           const char*,
                           const char*)
{
    if (action == SQLITE_TRANSACTION &&
        first_argument &&
        std::string(first_argument) == "ROLLBACK") {
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}

bool FileExists(const std::string& path)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    return input.good();
}

#ifdef _WIN32
std::string WidePathToUtf8(const std::wstring& path)
{
    if (path.empty()) return std::string();
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, path.c_str(), -1, NULL, 0, NULL, NULL);
    if (length <= 1) return std::string();
    std::string result(static_cast<size_t>(length), '\0');
    if (!WideCharToMultiByte(
            CP_UTF8, 0, path.c_str(), -1, &result[0], length, NULL, NULL)) {
        return std::string();
    }
    result.resize(static_cast<size_t>(length - 1));
    return result;
}

bool TestUnicodeDatabasePath()
{
    wchar_t temporary_path[MAX_PATH] = {0};
    const DWORD length = GetTempPathW(MAX_PATH, temporary_path);
    if (length == 0 || length >= MAX_PATH) return false;

    std::wstring directory(temporary_path, length);
    if (!directory.empty() &&
        directory[directory.size() - 1] != L'\\' &&
        directory[directory.size() - 1] != L'/') {
        directory += L'\\';
    }
    directory += L"BiblioLED_Unicode_";
    directory += std::to_wstring(static_cast<unsigned long>(GetTickCount()));
    directory += L"_ação";

    if (!CreateDirectoryW(directory.c_str(), NULL)) return false;

    const std::wstring backup_directory = directory + L"\\backups";
    if (!CreateDirectoryW(backup_directory.c_str(), NULL)) {
        RemoveDirectoryW(directory.c_str());
        return false;
    }

    const std::wstring database_path = directory + L"\\biblioteca.sqlite";
    const std::string database_utf8 = WidePathToUtf8(database_path);
    const std::string backup_directory_utf8 = WidePathToUtf8(backup_directory);
    std::string error;

    bool ok = !database_utf8.empty() && !backup_directory_utf8.empty();
    biblioled::LoanStore first;
    if (ok) ok = first.Open(database_utf8, backup_directory_utf8, &error);
    if (ok) {
        biblioled::LoanRecord record;
        record.title = "Registo num caminho Unicode";
        record.loan_date = "2099-03-01 10:00";
        record.type = "livro";
        ok = first.Insert(&record, &error);
    }
    first.Close();

    // Reopening proves that the UTF-8 database path is recognised as existing.
    // The insert also exercises compressed backup creation and retention in a
    // second directory whose path contains non-ASCII characters.
    biblioled::LoanStore second;
    std::vector<biblioled::LoanRecord> records;
    if (ok) ok = second.Open(database_utf8, backup_directory_utf8, &error);
    if (ok) ok = second.GetAll(&records, &error) &&
                 records.size() == 1 &&
                 records[0].title == "Registo num caminho Unicode";
    second.Close();

    WIN32_FIND_DATAW data;
    const std::wstring pattern = backup_directory + L"\\*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                DeleteFileW((backup_directory + L"\\" + data.cFileName).c_str());
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    DeleteFileW((database_path + L"-journal").c_str());
    DeleteFileW((database_path + L"-wal").c_str());
    DeleteFileW((database_path + L"-shm").c_str());
    const BOOL backup_directory_removed = RemoveDirectoryW(backup_directory.c_str());
    const BOOL database_deleted = DeleteFileW(database_path.c_str());
    const BOOL directory_removed = RemoveDirectoryW(directory.c_str());
    return ok && backup_directory_removed && database_deleted && directory_removed;
}
#endif

bool MakeDirectory(const std::string& path)
{
#ifdef _WIN32
    if (CreateDirectoryA(path.c_str(), NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    if (mkdir(path.c_str(), 0700) == 0) return true;
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

std::string CurrentDateStamp()
{
    const std::time_t now = std::time(NULL);
    const std::tm* local = std::localtime(&now);
    if (!local) return std::string();
    char date[16] = {0};
    if (std::strftime(date, sizeof(date), "%Y%m%d", local) == 0) return std::string();
    return date;
}

bool HasFileWithPrefix(const std::string& directory, const std::string& prefix)
{
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    const std::string pattern = directory + "\\" + prefix + "*";
    HANDLE find = FindFirstFileA(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            found = true;
            break;
        }
    } while (FindNextFileA(find, &data));
    FindClose(find);
    return found;
#else
    DIR* dir = opendir(directory.c_str());
    if (!dir) return false;
    struct dirent* entry = NULL;
    bool found = false;
    while ((entry = readdir(dir)) != NULL) {
        const std::string name(entry->d_name);
        if (name.compare(0, prefix.size(), prefix) == 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
#endif
}

bool HasFileWithPrefixAndSuffix(const std::string& directory,
                              const std::string& prefix,
                              const std::string& suffix)
{
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    const std::string pattern = directory + "\\" + prefix + "*";
    HANDLE find = FindFirstFileA(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const std::string name(data.cFileName);
            if (name.size() >= suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                found = true;
                break;
            }
        }
    } while (FindNextFileA(find, &data));
    FindClose(find);
    return found;
#else
    DIR* dir = opendir(directory.c_str());
    if (!dir) return false;
    struct dirent* entry = NULL;
    bool found = false;
    while ((entry = readdir(dir)) != NULL) {
        const std::string name(entry->d_name);
        if (name.compare(0, prefix.size(), prefix) == 0 &&
            name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
#endif
}

bool RemoveFilesWithPrefix(const std::string& directory, const std::string& prefix)
{
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    const std::string pattern = directory + "\\" + prefix + "*";
    HANDLE find = FindFirstFileA(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const std::string path = directory + "\\" + data.cFileName;
            if (!DeleteFileA(path.c_str())) ok = false;
        }
    } while (FindNextFileA(find, &data));
    FindClose(find);
    return ok;
#else
    DIR* dir = opendir(directory.c_str());
    if (!dir) return false;
    struct dirent* entry = NULL;
    bool ok = true;
    while ((entry = readdir(dir)) != NULL) {
        const std::string name(entry->d_name);
        if (name.compare(0, prefix.size(), prefix) == 0) {
            if (std::remove((directory + "/" + name).c_str()) != 0) ok = false;
        }
    }
    closedir(dir);
    return ok;
#endif
}

bool CreateUnrelatedSqliteFile(const std::string& path)
{
    sqlite3* db = NULL;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    const bool ok = Execute(db,
        "CREATE TABLE unrelated_data (value TEXT NOT NULL);"
        "INSERT INTO unrelated_data (value) VALUES ('valid SQLite, wrong application schema');"
        "PRAGMA user_version = 99;");
    const int close_rc = sqlite3_close(db);
    return ok && close_rc == SQLITE_OK;
}

bool CreateLegacyDatabase(const std::string& path, int version)
{
    sqlite3* db = NULL;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    bool ok = Execute(db,
        "CREATE TABLE loans ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " title TEXT NOT NULL,"
        " loan_date TEXT NOT NULL,"
        " type TEXT NOT NULL"
        ");");

    sqlite3_stmt* statement = NULL;
    if (ok) {
        ok = sqlite3_prepare_v2(
            db,
            "INSERT INTO loans (title, loan_date, type) VALUES (?1, ?2, ?3);",
            -1, &statement, NULL) == SQLITE_OK;
    }
    if (ok) {
        sqlite3_bind_text(statement, 1, "Registo legado", -1, SQLITE_STATIC);
        sqlite3_bind_text(statement, 2, "2024-01-15 10:30", -1, SQLITE_STATIC);
        sqlite3_bind_text(statement, 3, "livro", -1, SQLITE_STATIC);
        ok = sqlite3_step(statement) == SQLITE_DONE;
    }
    if (statement) sqlite3_finalize(statement);

    char pragma[64] = {0};
    std::snprintf(pragma, sizeof(pragma), "PRAGMA user_version = %d;", version);
    if (ok) ok = Execute(db, pragma);

    const int close_rc = sqlite3_close(db);
    return ok && close_rc == SQLITE_OK;
}

bool VerifyFutureVersionDatabaseUnchanged(const std::string& path)
{
    sqlite3* db = NULL;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    bool ok = true;
    int version = -1;
    sqlite3_stmt* statement = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA user_version;", -1,
                                &statement, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) {
            version = sqlite3_column_int(statement, 0);
        } else {
            ok = false;
        }
    } else {
        ok = false;
    }
    if (statement && sqlite3_finalize(statement) != SQLITE_OK) ok = false;
    statement = NULL;

    std::string title;
    std::string loan_date;
    std::string type;
    rc = sqlite3_prepare_v2(
        db, "SELECT title, loan_date, type FROM loans WHERE id = 1;",
        -1, &statement, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) {
            const unsigned char* title_text = sqlite3_column_text(statement, 0);
            const unsigned char* date_text = sqlite3_column_text(statement, 1);
            const unsigned char* type_text = sqlite3_column_text(statement, 2);
            if (title_text && date_text && type_text) {
                title = reinterpret_cast<const char*>(title_text);
                loan_date = reinterpret_cast<const char*>(date_text);
                type = reinterpret_cast<const char*>(type_text);
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
    } else {
        ok = false;
    }
    if (statement && sqlite3_finalize(statement) != SQLITE_OK) ok = false;

    const int close_result = sqlite3_close(db);
    return ok && version == 7 &&
           title == "Registo legado" &&
           loan_date == "2024-01-15 10:30" &&
           type == "livro" &&
           close_result == SQLITE_OK;
}

bool CreateDatabaseWithSchema(const std::string& path, const char* schema_sql)
{
    sqlite3* db = NULL;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    bool ok = Execute(db, schema_sql);
    if (ok) {
        ok = Execute(db,
            "INSERT INTO loans (id, title, loan_date, type) "
            "VALUES (1, 'Registo legado preservado', '2024-01-15 10:30', 'livro');");
    }
    if (ok) ok = Execute(db, "PRAGMA user_version = 0;");

    const int close_rc = sqlite3_close(db);
    return ok && close_rc == SQLITE_OK;
}

bool HasLegacyLoanTitle(const std::string& path)
{
    sqlite3* db = NULL;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    sqlite3_stmt* statement = NULL;
    const int prepare_rc = sqlite3_prepare_v2(
        db, "SELECT title FROM loans WHERE id = 1;", -1, &statement, NULL);
    bool found = false;
    if (prepare_rc == SQLITE_OK) {
        const int step = sqlite3_step(statement);
        if (step == SQLITE_ROW) {
            const unsigned char* title = sqlite3_column_text(statement, 0);
            found = title && sqlite3_stricmp(
                reinterpret_cast<const char*>(title),
                "Registo legado preservado") == 0;
        }
    }
    if (statement) sqlite3_finalize(statement);
    const int close_rc = sqlite3_close(db);
    return found && close_rc == SQLITE_OK;
}

bool CreateDatabaseWithAuditSchema(const std::string& path,
                                   const char* audit_schema_sql)
{
    sqlite3* db = NULL;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    bool ok = Execute(db,
        "CREATE TABLE loans ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " title TEXT NOT NULL,"
        " loan_date TEXT NOT NULL,"
        " type TEXT NOT NULL"
        ");");
    if (ok) {
        ok = Execute(db,
            "INSERT INTO loans (id, title, loan_date, type) "
            "VALUES (1, 'Registo legado preservado', '2024-01-15 10:30', 'livro');");
    }
    if (ok) ok = Execute(db, audit_schema_sql);
    if (ok) ok = Execute(db, "PRAGMA user_version = 1;");

    const int close_rc = sqlite3_close(db);
    return ok && close_rc == SQLITE_OK;
}

bool ReadAll(biblioled::LoanStore& store,
             std::vector<biblioled::LoanRecord>* records,
             const char* message)
{
    std::string error;
    const bool ok = store.GetAll(records, &error);
    Check(ok, message);
    if (!ok) std::cerr << error << std::endl;
    return ok;
}

void RemoveDatabaseFiles(const std::string& path)
{
    std::remove(path.c_str());
    std::remove((path + "-journal").c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

bool MakeReadableBackup(const std::string& path, std::string* decoded_path)
{
    if (!decoded_path) return false;
    if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0) {
        *decoded_path = path + ".test-decoded.sqlite";
        std::string error;
        return biblioled::DecompressBackupFile(
            path, *decoded_path, biblioled::BackupCompressionFormat::Gzip, &error);
    }
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".zlib") == 0) {
        *decoded_path = path + ".test-decoded.sqlite";
        std::string error;
        return biblioled::DecompressBackupFile(
            path, *decoded_path, biblioled::BackupCompressionFormat::Zlib, &error);
    }
    *decoded_path = path;
    return true;
}


int PrepareInterruptedTransaction(const std::string& database,
                                   sqlite3** database_handle)
{
    if (!database_handle) return 70;
    *database_handle = NULL;

    sqlite3* db = NULL;
    if (sqlite3_open_v2(database.c_str(), &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 71;
    }

    if (!Execute(db, "PRAGMA journal_mode=DELETE;") ||
        !Execute(db, "PRAGMA synchronous=FULL;") ||
        !Execute(db, "PRAGMA cache_size=8;") ||
        !Execute(db, "PRAGMA cache_spill=ON;") ||
        !Execute(db, "BEGIN IMMEDIATE;")) {
        sqlite3_close(db);
        return 72;
    }

    sqlite3_stmt* statement = NULL;
    const char* sql =
        "INSERT INTO loans (title, loan_date, type) VALUES (?1, ?2, ?3);";
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 73;
    }

    // Force cache spill and journal synchronisation while the transaction is
    // still open. All rows are synthetic and should disappear after recovery.
    const std::string payload(4096, 'X');
    bool inserted = true;
    for (int i = 0; i < 128; ++i) {
        const std::string title =
            std::string("UNCOMMITTED_PROCESS_CRASH_") +
            std::to_string(i) + "_" + payload;

        if (sqlite3_bind_text(statement, 1, title.c_str(),
                              static_cast<int>(title.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement, 2, "2099-12-31 23:59", -1,
                              SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(statement, 3, "livro", -1,
                              SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_DONE ||
            sqlite3_reset(statement) != SQLITE_OK ||
            sqlite3_clear_bindings(statement) != SQLITE_OK) {
            inserted = false;
            break;
        }
    }

    const int finalize_result = sqlite3_finalize(statement);
    if (!inserted || finalize_result != SQLITE_OK ||
        sqlite3_get_autocommit(db) != 0 ||
        sqlite3_db_cacheflush(db) != SQLITE_OK ||
        sqlite3_get_autocommit(db) != 0) {
        sqlite3_close(db);
        return 74;
    }

    *database_handle = db;
    return 0;
}

#ifdef _WIN32
bool RunInterruptedTransactionProcess(const std::string& database,
                                      DWORD* child_exit_code)
{
    if (!child_exit_code) return false;
    *child_exit_code = 0;

    wchar_t executable_path[MAX_PATH] = {0};
    const DWORD path_length =
        GetModuleFileNameW(NULL, executable_path, MAX_PATH);
    if (path_length == 0 || path_length >= MAX_PATH) return false;

    std::wstring command_line = L"\"";
    command_line.append(executable_path, path_length);
    command_line += L"\" --sqlite-crash-worker \"";
    // The test database name is deliberately ASCII; the current directory is
    // inherited unchanged, including when that directory itself is Unicode.
    command_line.append(database.begin(), database.end());
    command_line += L"\"";

    STARTUPINFOW startup = {};
    PROCESS_INFORMATION process = {};
    startup.cb = sizeof(startup);

    if (!CreateProcessW(NULL, &command_line[0], NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL,
                        &startup, &process)) {
        return false;
    }

    const DWORD wait_result = WaitForSingleObject(process.hProcess, 30000);
    bool ok = wait_result == WAIT_OBJECT_0;
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 89);
        WaitForSingleObject(process.hProcess, INFINITE);
    }

    if (ok) ok = GetExitCodeProcess(process.hProcess, child_exit_code) != 0;

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ok;
}
#else
bool RunInterruptedTransactionProcess(const std::string& database,
                                      int* child_exit_code)
{
    if (!child_exit_code) return false;
    *child_exit_code = 0;

    const pid_t child = fork();
    if (child < 0) return false;

    if (child == 0) {
        sqlite3* child_database = NULL;
        const int result =
            PrepareInterruptedTransaction(database, &child_database);
        if (result != 0) _exit(result);

        // Deliberately bypass SQLite close/finalisers to model process death.
        _exit(86);
    }

    int wait_status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(child, &wait_status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited != child || !WIFEXITED(wait_status)) return false;
    *child_exit_code = WEXITSTATUS(wait_status);
    return true;
}
#endif

void TestAbruptProcessInterruptionRecovery(const std::string& database,
                                           const std::string& backup_directory)
{
    Check(MakeDirectory(backup_directory),
          "process-interruption backup directory should be created");

    std::string error;
    biblioled::LoanStore initial;
    Check(initial.Open(database, backup_directory, &error),
          "isolated database for process-interruption test should open");
    if (!initial.IsOpen()) return;

    biblioled::LoanRecord committed;
    committed.title = "Registo confirmado antes da interrupção";
    committed.loan_date = "2099-01-01 10:00";
    committed.type = "livro";
    Check(initial.Insert(&committed, &error),
          "a known committed record should be written before child termination");
    initial.Close();

#ifdef _WIN32
    DWORD child_exit_code = 0;
#else
    int child_exit_code = 0;
#endif
    const bool child_started_and_exited =
        RunInterruptedTransactionProcess(database, &child_exit_code);
    Check(child_started_and_exited,
          "the SQLite crash-worker process should start and terminate");
    Check(child_exit_code == 86,
          "the crash-worker should terminate abruptly from inside its open transaction");

    // A hot rollback journal must survive process termination and be consumed
    // by SQLite when the parent opens the database again.
    Check(FileExists(database + "-journal"),
          "abrupt process termination should leave a hot rollback journal");

    biblioled::LoanStore recovered;
    error.clear();
    Check(recovered.Open(database, backup_directory, &error),
          "LoanStore should reopen and recover the database after child termination");
    if (recovered.IsOpen()) {
        std::vector<biblioled::LoanRecord> records;
        Check(recovered.GetAll(&records, &error),
              "recovered database should remain readable");
        Check(records.size() == 1,
              "recovery should retain only the previously committed record");
        if (records.size() == 1) {
            Check(records[0].title ==
                      "Registo confirmado antes da interrupção",
                  "recovery should preserve the exact committed record");
        }
        Check(recovered.QuickCheck(&error),
              "database should pass quick_check after process-interruption recovery");
        Check(!FileExists(database + "-journal"),
              "SQLite should remove the hot journal after successful recovery");
        recovered.Close();
    }

    RemoveDatabaseFiles(database);
    RemoveFilesWithPrefix(backup_directory, "BiblioLED_");
#ifdef _WIN32
    RemoveDirectoryA(backup_directory.c_str());
#else
    rmdir(backup_directory.c_str());
#endif
}

} // namespace

void TestRollbackFailureClosesStore(const std::string& database,
                                   const std::string& backup_directory)
{
    Check(MakeDirectory(backup_directory),
          "rollback-failure backup directory should be created");

    biblioled::LoanStore store;
    std::string error;
    Check(store.Open(database, backup_directory, &error),
          "isolated database for rollback-failure injection should open");
    if (!store.IsOpen()) return;

    sqlite3* raw = biblioled::LoanStoreTestAccess::Database(store);
    Check(raw != NULL, "test adapter should expose the active SQLite handle");
    if (!raw) {
        store.Close();
        return;
    }

    Check(Execute(raw, "BEGIN IMMEDIATE;"),
          "rollback-failure test should start an explicit transaction");
    Check(Execute(raw,
        "INSERT INTO loans (title, loan_date, type) "
        "VALUES ('Registo não confirmado', '2099-04-01 10:00', 'livro');"),
        "rollback-failure test should create an uncommitted loan row");
    Check(sqlite3_get_autocommit(raw) == 0,
          "the injected transaction should remain active before rollback");

    const int authorizer_result =
        sqlite3_set_authorizer(raw, DenyRollbackAuthorizer, NULL);
    Check(authorizer_result == SQLITE_OK,
          "SQLite should accept the rollback-denying authorizer");

    if (authorizer_result == SQLITE_OK) {
        error.clear();
        Check(!biblioled::LoanStoreTestAccess::RollbackTransaction(store, &error),
              "a denied ROLLBACK statement should be reported as a rollback failure");
        Check(!store.IsOpen(),
              "LoanStore should close itself after an unsuccessful ROLLBACK");
        Check(error.find("SQLite rollback failed") != std::string::npos,
              "rollback failure should be included in the returned diagnostic");
    } else {
        store.Close();
    }

    if (store.IsOpen()) store.Close();

    // Reopening forces SQLite to recover any journal left by the deliberately
    // interrupted transaction. The uncommitted row must never become visible.
    biblioled::LoanStore reopened;
    error.clear();
    Check(reopened.Open(database, backup_directory, &error),
          "database should reopen after the failed rollback closed its connection");
    if (reopened.IsOpen()) {
        std::vector<biblioled::LoanRecord> records;
        Check(reopened.GetAll(&records, &error),
              "recovered database should remain readable after failed rollback");
        Check(records.empty(),
              "SQLite recovery should discard the uncommitted row");
        Check(reopened.QuickCheck(&error),
              "recovered database should pass SQLite quick_check");
        reopened.Close();
    }

    RemoveDatabaseFiles(database);
    RemoveFilesWithPrefix(backup_directory, "BiblioLED_");
#ifdef _WIN32
    RemoveDirectoryA(backup_directory.c_str());
#else
    rmdir(backup_directory.c_str());
#endif
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--sqlite-crash-worker") {
        sqlite3* child_database = NULL;
        const int result =
            PrepareInterruptedTransaction(argv[2], &child_database);
        if (result != 0) return result;

#ifdef _WIN32
        if (TerminateProcess(GetCurrentProcess(), 86)) return 87;
        sqlite3_close(child_database);
        return 88;
#else
        // Deliberately bypass SQLite close/finalisers to model process death.
        _exit(86);
#endif
    }

    const std::string database =
        "loan-store-test-" + std::to_string(std::rand()) + ".sqlite";
    const std::string blocked_database = database + ".blocked.sqlite";
    const std::string no_op_database = database + ".no-op.sqlite";
    const std::string legacy_database = database + ".legacy.sqlite";
    const std::string failed_migration_database = database + ".failed-migration.sqlite";
    const std::string failed_ddl_migration_database =
        database + ".failed-ddl-migration.sqlite";
    const std::string failed_ddl_migration_backup_dir =
        database + ".failed-ddl-migration-backups";
    const std::string unsupported_database = database + ".unsupported.sqlite";
    const std::string malformed_v1_database = database + ".malformed-v1.sqlite";
    const std::string malformed_audit_type_database = database + ".malformed-audit-type.sqlite";
    const std::string malformed_audit_nullable_database = database + ".malformed-audit-nullable.sqlite";
    const std::string malformed_audit_extra_database = database + ".malformed-audit-extra.sqlite";
    const std::string malformed_legacy_type_database = database + ".malformed-legacy-type.sqlite";
    const std::string malformed_legacy_key_database = database + ".malformed-legacy-key.sqlite";
    const std::string malformed_legacy_nullable_database = database + ".malformed-legacy-nullable.sqlite";
    const std::string malformed_legacy_extra_database = database + ".malformed-legacy-extra.sqlite";
    const std::string missing_backup_directory = database + ".missing-backup-directory";
    const std::string invalid_backup = database + ".invalid-backup.sqlite";
    const std::string rollback_failure_database =
        database + ".rollback-failure.sqlite";
    const std::string rollback_failure_backup_directory =
        database + ".rollback-failure-backups";
    const std::string process_interruption_database =
        database + ".process-interruption.sqlite";
    const std::string process_interruption_backup_directory =
        database + ".process-interruption-backups";

    std::vector<std::string> backups;
    std::string error;

#ifdef _WIN32
    Check(TestUnicodeDatabasePath(),
          "LoanStore should reopen a database under a UTF-8 path with non-ASCII characters");
#endif

    TestRollbackFailureClosesStore(
        rollback_failure_database, rollback_failure_backup_directory);
    TestAbruptProcessInterruptionRecovery(
        process_interruption_database, process_interruption_backup_directory);

    // A new database starts at schema version 1 with an empty audit table.
    biblioled::LoanStore store;
    Check(store.Open(database, ".", &error), "a new isolated database should open");
    if (!store.IsOpen()) {
        std::cerr << error << std::endl;
        return 1;
    }
    Check(store.SchemaVersion() == 1, "new database should use schema version 1");
    Check(!store.MigrationRequired(), "new database should not require migration");

    std::vector<biblioled::LoanRecord> records;
    ReadAll(store, &records, "an empty database should be readable");
    Check(records.empty(), "a new database should contain no loan records");

    std::vector<biblioled::AuditEvent> events;
    Check(store.GetAuditEvents(&events, &error), "new audit table should be readable");
    Check(events.empty(), "new database should start with no audit events");
    Check(!store.HasSessionBackup(), "opening a database should not create a backup");

    // The persistence layer rejects invalid/non-canonical dates before
    // backups, writes, or audit events are created.
    const char* invalid_dates[] = {
        "2024-2-29 12:30",
        "2024-02-30 12:30",
        "2024-02-29 2:30",
        "2024-02-29 12:30 "
    };
    for (size_t i = 0; i < sizeof(invalid_dates) / sizeof(invalid_dates[0]); ++i) {
        biblioled::LoanRecord invalid_date_record;
        invalid_date_record.title = "Invalid date regression";
        invalid_date_record.loan_date = invalid_dates[i];
        invalid_date_record.type = "livro";
        Check(!store.Insert(&invalid_date_record, &error),
              "LoanStore must reject invalid or non-canonical dates");
        Check(!store.HasSessionBackup(),
              "rejected invalid-date insert must not create an automatic backup");
    }
    Check(store.GetAll(&records, &error) && records.empty(),
          "invalid-date inserts must leave the loans table unchanged");
    Check(store.GetAuditEvents(&events, &error) && events.empty(),
          "invalid-date inserts must not create audit events");

    // Missing record identifiers are rejected before the automatic-backup gate.
    biblioled::LoanStore no_op_store;
    Check(no_op_store.Open(no_op_database, ".", &error),
          "isolated no-op database should open");
    if (no_op_store.IsOpen()) {
        biblioled::LoanRecord missing_record;
        missing_record.id = 42;
        missing_record.title = "Registo inexistente";
        missing_record.loan_date = "2099-01-01 10:00";
        missing_record.type = "livro";
        Check(!no_op_store.Update(missing_record, &error),
              "updating a missing record should fail");
        Check(!no_op_store.HasSessionBackup(),
              "updating a missing record must not create a daily backup");
        Check(!no_op_store.Remove(42, &error),
              "deleting a missing record should fail");
        Check(!no_op_store.HasSessionBackup(),
              "deleting a missing record must not create a daily backup");
        no_op_store.Close();
    }
    RemoveDatabaseFiles(no_op_database);

    // Audit-only writes (such as startup/shutdown) must not create automatic backups.
    const std::string audit_only_database = database + ".audit-only.sqlite";
    biblioled::LoanStore audit_only_store;
    Check(audit_only_store.Open(audit_only_database, ".", &error),
          "an isolated audit-only database should open");
    if (audit_only_store.IsOpen()) {
        Check(audit_only_store.RecordAuditEvent("startup", "test session", &error),
              "an audit-only lifecycle event should be recorded");
        Check(!audit_only_store.HasSessionBackup(),
              "an audit-only event must not trigger an automatic loan-data backup");
        audit_only_store.Close();
    }
    RemoveDatabaseFiles(audit_only_database);

    // An existing same-day SQLite file must not be reused unless it is a valid
    // BiblioLED schema. Preserve an unrelated but structurally valid SQLite file.
    const std::string candidate_backup_dir = database + ".candidate-backups";
    const std::string candidate_database = database + ".candidate.sqlite";
    Check(MakeDirectory(candidate_backup_dir),
          "isolated daily-candidate backup directory should be created");
    const std::string candidate_date = CurrentDateStamp();
    const std::string candidate_prefix = "BiblioLED_auto_" + candidate_date;
    const std::string unrelated_candidate =
        candidate_backup_dir + "/" + candidate_prefix + ".sqlite";
    Check(CreateUnrelatedSqliteFile(unrelated_candidate),
          "unrelated but integrity-valid SQLite candidate should be created");
    {
        std::ofstream stale_wal((unrelated_candidate + "-wal").c_str(),
                                std::ios::out | std::ios::binary);
        stale_wal << "stale WAL sidecar from an invalid automatic backup";
    }
    Check(FileExists(unrelated_candidate + "-wal"),
          "invalid automatic backup WAL sidecar should be created");
    biblioled::LoanStore candidate_store;
    Check(candidate_store.Open(candidate_database, candidate_backup_dir, &error),
          "daily-candidate test database should open");
    if (candidate_store.IsOpen()) {
        biblioled::LoanRecord candidate_loan;
        candidate_loan.title = "Candidate schema validation";
        candidate_loan.loan_date = "2099-01-01 10:00";
        candidate_loan.type = "livro";
        Check(candidate_store.Insert(&candidate_loan, &error),
              "loan mutation should create a proper daily snapshot when the candidate schema is invalid");
        Check(!FileExists(unrelated_candidate),
              "unrelated same-day SQLite file must be moved out of the automatic namespace");
        Check(!FileExists(unrelated_candidate + "-wal"),
              "invalid automatic backup WAL must move with its database file");
        Check(HasFileWithPrefixAndSuffix(
                  candidate_backup_dir,
                  "BiblioLED_invalid_auto_" + candidate_date + "_",
                  ".sqlite-wal"),
              "invalid automatic backup WAL sidecar should remain preserved beside the renamed database");
        Check(HasFileWithPrefix(candidate_backup_dir,
                                "BiblioLED_invalid_auto_" + candidate_date + "_"),
              "unrelated SQLite candidate should be preserved under an invalid-backup name");
        Check(candidate_store.LastSessionBackupPath().find(".sqlite.gz") != std::string::npos,
              "a valid compressed BiblioLED daily snapshot should replace the invalid candidate");
        const std::string candidate_daily_backup = candidate_store.LastSessionBackupPath();
        candidate_store.Close();
        RemoveDatabaseFiles(candidate_daily_backup);
    }
    RemoveDatabaseFiles(candidate_database);
    RemoveFilesWithPrefix(candidate_backup_dir, "BiblioLED_invalid_auto_");
#ifdef _WIN32
    RemoveDirectoryA(candidate_backup_dir.c_str());
#else
    rmdir(candidate_backup_dir.c_str());
#endif

    // CRUD writes and audit rows must commit or roll back together.
    biblioled::LoanRecord book;
    book.title = "Açúcar e café";
    book.loan_date = "2099-01-10 12:30";
    book.type = "livro";
    Check(store.Insert(&book, &error), "insert should succeed");
    Check(book.id > 0, "insert should return the generated identifier");
    Check(store.HasSessionBackup(), "first mutation should create a session backup");

    const std::string first_backup = store.LastSessionBackupPath();
    Check(!first_backup.empty(), "session backup path should be reported");
#ifdef _WIN32
    Check(first_backup.find('/') == std::string::npos,
          "Windows automatic backup paths should use backslashes only");
#endif
    backups.push_back(first_backup);

    std::string first_backup_decoded;
    Check(MakeReadableBackup(first_backup, &first_backup_decoded),
          "daily backup should decompress for inspection");
    biblioled::LoanStore backup_reader;
    Check(backup_reader.Open(first_backup_decoded, ".", &error),
          "decompressed daily backup should open as a SQLite database");
    if (backup_reader.IsOpen()) {
        ReadAll(backup_reader, &records, "session backup should be readable");
        Check(records.empty(), "first session backup must represent the database before insertion");
        Check(backup_reader.SchemaVersion() == 1,
              "session backup should preserve the schema version");
        backup_reader.Close();
    }
    if (first_backup_decoded != first_backup) RemoveDatabaseFiles(first_backup_decoded);

    biblioled::LoanRecord audio;
    audio.title = "Audiolivro de teste";
    audio.loan_date = "2099-01-11 09:15";
    audio.type = "audiolivro";
    Check(store.Insert(&audio, &error), "second insert should succeed");
    Check(store.LastSessionBackupPath() == first_backup,
          "a session should reuse its first backup for later mutations");

    Check(store.GetById(book.id, &book, &error), "lookup by id should succeed");
    Check(book.title == "Açúcar e café", "UTF-8 title should round-trip");
    Check(store.GetAll(&records, &error) && records.size() == 2,
          "list should return both inserted records");

    biblioled::LoanRecord invalid_update = book;
    invalid_update.loan_date = "2024-02-30 12:30";
    Check(!store.Update(invalid_update, &error),
          "LoanStore must reject invalid update dates");
    Check(store.GetById(book.id, &book, &error) &&
          book.loan_date == "2099-01-10 12:30",
          "invalid-date update must leave the original record unchanged");
    Check(store.GetAuditEvents(&events, &error) && events.size() == 2,
          "invalid-date update must not add an audit event");

    biblioled::LoanRecord edited = book;
    edited.title = "Título alterado";
    edited.type = "livro";
    Check(store.Update(edited, &error), "update should succeed");
    Check(store.GetById(book.id, &book, &error), "updated record should be readable");
    Check(book.title == "Título alterado", "updated title should be stored");
    Check(store.LastSessionBackupPath() == first_backup,
          "update in the same session should not create a second backup");

    Check(store.GetAuditEvents(&events, &error), "audit events should be readable after CRUD");
    Check(events.size() == 3, "two inserts and one update should create three audit events");
    if (events.size() == 3) {
        Check(events[0].action == "create" && events[0].has_after,
              "first audit event should describe record creation");
        Check(events[0].after.title == "Açúcar e café",
              "create audit event should retain the after value");
        Check(events[2].action == "update" && events[2].has_before && events[2].has_after,
              "update audit event should retain before and after values");
        Check(events[2].before.title == "Açúcar e café" &&
              events[2].after.title == "Título alterado",
              "update audit event should contain the correct before/after titles");
    }

    // A manual backup is distinct and preserves both loan and audit history.
    std::string manual_backup;
    Check(store.CreateManualBackup(&manual_backup, &error),
          "manual backup should be created and validated");
    Check(!manual_backup.empty() && manual_backup != first_backup,
          "manual backup should be a separate file");
#ifdef _WIN32
    Check(manual_backup.find('/') == std::string::npos,
          "Windows manual backup paths should use backslashes only");
#endif
    backups.push_back(manual_backup);

    std::string manual_backup_decoded;
    Check(MakeReadableBackup(manual_backup, &manual_backup_decoded),
          "manual backup should decompress for inspection");
    biblioled::LoanStore manual_reader;
    Check(manual_reader.Open(manual_backup_decoded, ".", &error), "decompressed manual backup should open");
    if (manual_reader.IsOpen()) {
        Check(manual_reader.GetAll(&records, &error) && records.size() == 2,
              "manual backup should contain the current records");
        Check(manual_reader.GetAuditEvents(&events, &error) && events.size() == 3,
              "manual backup should contain the matching audit history");
        manual_reader.Close();
    }
    // Generate a second manual snapshot with the alternative standard wrapper.
    store.SetBackupCompression("zlib", 9);
    std::string zlib_manual_backup;
    Check(store.CreateManualBackup(&zlib_manual_backup, &error),
          "zlib-format manual backup should be created and validated");
    Check(zlib_manual_backup.size() >= 12 &&
          zlib_manual_backup.compare(zlib_manual_backup.size() - 12, 12, ".sqlite.zlib") == 0,
          "zlib-format backup should use the .sqlite.zlib extension");
#ifdef _WIN32
    Check(zlib_manual_backup.find('/') == std::string::npos,
          "Windows zlib backup paths should use backslashes only");
#endif
    backups.push_back(zlib_manual_backup);

    // Change settings again while the store remains open; the next manual
    // backup must use the new format and level without reopening the store.
    store.SetBackupCompression("gzip", 1);
    std::string immediate_gzip_backup;
    Check(store.CreateManualBackup(&immediate_gzip_backup, &error),
          "a live compression-setting change should affect the next backup");
    Check(immediate_gzip_backup.size() >= 10 &&
          immediate_gzip_backup.compare(immediate_gzip_backup.size() - 10, 10, ".sqlite.gz") == 0,
          "a live change back to gzip should take effect immediately");
    backups.push_back(immediate_gzip_backup);

    std::string zlib_manual_decoded;
    Check(MakeReadableBackup(zlib_manual_backup, &zlib_manual_decoded),
          "zlib-format manual backup should decompress for inspection");
    biblioled::LoanStore zlib_manual_reader;
    Check(zlib_manual_reader.Open(zlib_manual_decoded, ".", &error),
          "decompressed zlib-format manual backup should open");
    if (zlib_manual_reader.IsOpen()) {
        Check(zlib_manual_reader.GetAll(&records, &error) && records.size() == 2,
              "zlib-format manual backup should contain both loans");
        zlib_manual_reader.Close();
    }
    if (zlib_manual_decoded != zlib_manual_backup) RemoveDatabaseFiles(zlib_manual_decoded);

    int count = 0;
    Check(store.CountSince("2099-01-01 00:00", &count, &error),
          "count since date should succeed");
    Check(count == 2, "future-dated records should be included in quota counts");
    store.Close();

    // A second session on the same day reuses the existing daily snapshot.
    biblioled::LoanStore reopened;
    Check(reopened.Open(database, ".", &error), "database should reopen");
    Check(reopened.SchemaVersion() == 1, "reopened database should retain schema version 1");
    Check(!reopened.MigrationRequired(), "reopened database should not require migration");
    Check(!reopened.HasSessionBackup(), "a new session should start without a backup");
    Check(reopened.GetById(book.id, &book, &error), "record should be readable after reopen");

    const std::string before_update = book.title;
    edited = book;
    edited.title = "Alteração na segunda sessão";
    Check(reopened.Update(edited, &error), "update in a new session should succeed");
    Check(reopened.HasSessionBackup(), "first update in a new session should create a backup");

    const std::string second_backup = reopened.LastSessionBackupPath();
    Check(second_backup == first_backup,
          "a new session on the same calendar day should reuse the daily automatic backup");
    Check(book.title == before_update,
          "the pre-update loan should remain readable before it is changed again");

    Check(reopened.Remove(audio.id, &error), "delete should succeed");
    Check(reopened.LastSessionBackupPath() == second_backup,
          "delete in the same session should reuse the session backup");
    Check(!reopened.GetById(audio.id, &book, &error),
          "deleted record should no longer be found");
    Check(reopened.GetAuditEvents(&events, &error) && events.size() == 5,
          "delete should append a fifth audit event");
    if (events.size() == 5) {
        Check(events[4].action == "delete" && events[4].has_before && !events[4].has_after,
              "delete audit event should retain before values only");
        Check(events[4].before.title == "Audiolivro de teste",
              "delete audit event should preserve the removed title");
    }

    // Restore returns both loans and internal audit history to the selected snapshot.
    std::string pre_restore_backup;
    Check(reopened.RestoreFromBackup(manual_backup, &pre_restore_backup, &error),
          "restore from a valid backup should succeed");
    Check(!pre_restore_backup.empty() && pre_restore_backup != manual_backup,
          "restore must create a separate safety copy of the current database");
    backups.push_back(pre_restore_backup);

    std::string pre_restore_decoded;
    Check(MakeReadableBackup(pre_restore_backup, &pre_restore_decoded),
          "pre-restore safety backup should decompress for inspection");
    biblioled::LoanStore pre_restore_reader;
    Check(pre_restore_reader.Open(pre_restore_decoded, ".", &error),
          "decompressed pre-restore safety copy should open");
    if (pre_restore_reader.IsOpen()) {
        Check(pre_restore_reader.GetAll(&records, &error) && records.size() == 1,
              "pre-restore safety copy should contain the state before restore");
        Check(pre_restore_reader.GetAuditEvents(&events, &error) && events.size() == 5,
              "pre-restore safety copy should retain the matching audit history");
        pre_restore_reader.Close();
    }
    if (pre_restore_decoded != pre_restore_backup) RemoveDatabaseFiles(pre_restore_decoded);

    Check(reopened.GetAll(&records, &error) && records.size() == 2,
          "restore should replace the database with the selected snapshot");
    Check(reopened.GetAuditEvents(&events, &error) && events.size() == 3,
          "restore should replace audit history with the snapshot history");
    Check(reopened.GetById(book.id, &book, &error),
          "restored database should contain the original record");
    Check(book.title == "Título alterado",
          "restore should recover the title captured in the selected snapshot");
    Check(reopened.GetById(audio.id, &book, &error),
          "restore should recover records removed after the selected snapshot");

    std::string zlib_pre_restore;
    Check(reopened.RestoreFromBackup(zlib_manual_backup, &zlib_pre_restore, &error),
          "restore should accept a zlib-wrapped backup");
    Check(!zlib_pre_restore.empty(), "zlib restore should create a safety backup");
    backups.push_back(zlib_pre_restore);

    std::string legacy_pre_restore;
    Check(reopened.RestoreFromBackup(manual_backup_decoded, &legacy_pre_restore, &error),
          "restore should continue accepting an uncompressed legacy .sqlite backup");
    Check(!legacy_pre_restore.empty(), "legacy .sqlite restore should create a safety backup");
    backups.push_back(legacy_pre_restore);
    if (manual_backup_decoded != manual_backup) RemoveDatabaseFiles(manual_backup_decoded);

    // A failed audit insert must roll back the corresponding loan insert.
    sqlite3* raw = NULL;
    Check(sqlite3_open(database.c_str(), &raw) == SQLITE_OK,
          "raw SQLite connection should open for failure injection");
    if (raw) {
        Check(Execute(raw,
            "CREATE TRIGGER fail_audit_insert BEFORE INSERT ON audit_events "
            "BEGIN SELECT RAISE(ABORT, 'audit write blocked'); END;"),
            "audit failure trigger should be created");

        biblioled::LoanRecord rejected;
        rejected.title = "Não deve ficar gravado";
        rejected.loan_date = "2099-02-01 10:00";
        rejected.type = "livro";
        Check(!reopened.Insert(&rejected, &error),
              "loan insert should fail when its audit row cannot be inserted");
        Check(rejected.id == 0, "failed insert should not expose a generated identifier");
        Check(reopened.GetAll(&records, &error) && records.size() == 2,
              "failed audit write should roll back the loan insert");
        Check(reopened.GetAuditEvents(&events, &error) && events.size() == 3,
              "failed audit write should not leave an orphan audit event");

        Check(Execute(raw, "DROP TRIGGER fail_audit_insert;"),
              "audit failure trigger should be removed");
        sqlite3_close(raw);
        raw = NULL;
    }
    if (raw) sqlite3_close(raw);

    // Enabled external logging blocks later mutations after a sync failure,
    // while deliberately disabled file logging does not block internal audit.
    reopened.SetExternalAuditLogStatus(true, false);
    Check(reopened.ExternalAuditLogBlocked(),
          "enabled but unavailable external logging should expose blocked state");
    biblioled::LoanRecord log_blocked;
    log_blocked.title = "Bloqueado pelo log";
    log_blocked.loan_date = "2099-02-02 10:00";
    log_blocked.type = "livro";
    Check(!reopened.Insert(&log_blocked, &error),
          "unavailable enabled external logging should block a loan mutation");
    Check(reopened.GetAll(&records, &error) && records.size() == 2,
          "blocked mutation should leave loans unchanged");

    Check(reopened.GetAuditEvents(&events, &error),
          "internal diagnostics should remain readable while external logging is blocked");
    const size_t diagnostic_count_before = events.size();
    Check(reopened.RecordDiagnosticEvent(
              "warning", "context=loan_dialog;message=missing_title", &error),
          "a warning should be queued internally while external logging is blocked");
    Check(reopened.RecordDiagnosticEvent(
              "error", "context=statistics;message=calculation_failed", &error),
          "an error should be queued internally while external logging is blocked");
    Check(reopened.RecordDiagnosticEvent(
              "failure", "context=backup;message=creation_failed", &error),
          "a failure should be queued internally while external logging is blocked");
    Check(!reopened.RecordDiagnosticEvent(
              "debug", "unsupported severity", &error),
          "unsupported diagnostic severities should be rejected");
    Check(reopened.ExternalAuditLogBlocked(),
          "queuing diagnostics must not clear the external-log blocked state");
    Check(!reopened.RecordAuditEvent(
              "settings_changed", "ordinary event remains blocked", &error),
          "ordinary audit events must remain blocked while external logging is unhealthy");
    Check(reopened.GetAuditEvents(&events, &error) &&
              events.size() == diagnostic_count_before + 3,
          "three valid diagnostic events should append exactly three rows");
    if (events.size() == diagnostic_count_before + 3) {
        Check(events[diagnostic_count_before].action == "application_warning" &&
                  events[diagnostic_count_before].details ==
                      "context=loan_dialog;message=missing_title",
              "warning diagnostic should retain its severity and context");
        Check(events[diagnostic_count_before + 1].action == "application_error" &&
                  events[diagnostic_count_before + 1].details ==
                      "context=statistics;message=calculation_failed",
              "error diagnostic should retain its severity and context");
        Check(events[diagnostic_count_before + 2].action == "application_failure" &&
                  events[diagnostic_count_before + 2].details ==
                      "context=backup;message=creation_failed",
              "failure diagnostic should retain its severity and context");
    }

    reopened.SetExternalAuditLogStatus(false, false);
    Check(!reopened.ExternalAuditLogBlocked(),
          "deliberately disabled external logging should not block mutations");
    const size_t events_before_disabled_insert = events.size();
    biblioled::LoanRecord logging_disabled;
    logging_disabled.title = "Registo com log externo desactivado";
    logging_disabled.loan_date = "2099-02-03 10:00";
    logging_disabled.type = "livro";
    Check(reopened.Insert(&logging_disabled, &error),
          "mutations should remain available while external logging is disabled");
    Check(reopened.GetAll(&records, &error) && records.size() == 3,
          "disabled external logging should still persist the loan");
    Check(reopened.GetAuditEvents(&events, &error) &&
              events.size() == events_before_disabled_insert + 1,
          "disabled external logging should still persist internal audit events");
    reopened.SetExternalAuditLogStatus(true, true);

    {
        std::ofstream invalid(invalid_backup.c_str(), std::ios::out | std::ios::binary);
        invalid << "This is not a SQLite database.";
    }
    Check(!reopened.RestoreFromBackup(invalid_backup, NULL, &error),
          "restore should reject a non-SQLite file");
    Check(reopened.GetAll(&records, &error) && records.size() == 3,
          "invalid restore source must not change the current database");
    Check(reopened.QuickCheck(&error), "quick_check should pass after restore and failure injection");

    Check(reopened.GetAuditEvents(&events, &error),
          "audit events should be readable before operational audit test");
    const size_t operational_event_count = events.size();
    Check(reopened.RecordAuditEvent("manual_backup", "path=isolated-test.sqlite", &error),
          "a generic operational audit event should be recorded");
    Check(reopened.GetAuditEvents(&events, &error) &&
          events.size() == operational_event_count + 1,
          "a generic operational event should append exactly one audit row");
    if (events.size() == operational_event_count + 1) {
        Check(events.back().action == "manual_backup" &&
              events.back().details == "path=isolated-test.sqlite" &&
              !events.back().has_before && !events.back().has_after,
              "an operational audit event should preserve its action and details without loan values");
    }

    sqlite3* audit_failure_db = NULL;
    Check(sqlite3_open(database.c_str(), &audit_failure_db) == SQLITE_OK,
          "a second connection should open for operational audit failure injection");
    if (audit_failure_db) {
        Check(Execute(audit_failure_db,
            "CREATE TRIGGER fail_audit_insert BEFORE INSERT ON audit_events "
            "BEGIN SELECT RAISE(ABORT, 'audit write blocked'); END;"),
            "operational audit failure trigger should be created");
        Check(!reopened.RecordAuditEvent("restore", "must roll back", &error),
              "an operational event should fail when its audit insert is rejected");
        Check(reopened.GetAuditEvents(&events, &error) &&
              events.size() == operational_event_count + 1,
              "a failed operational audit insert must not leave an audit row");
        Check(Execute(audit_failure_db, "DROP TRIGGER fail_audit_insert;"),
              "operational audit failure trigger should be removed");
        sqlite3_close(audit_failure_db);
    }

    reopened.Close();

    // Legacy schema v0 is readable but write-protected until confirmed migration.
    Check(CreateLegacyDatabase(legacy_database, 0),
          "legacy version-0 database should be created");
    biblioled::LoanStore legacy;
    Check(legacy.Open(legacy_database, ".", &error),
          "legacy version-0 database should open for read-only access");
    Check(legacy.SchemaVersion() == 0, "legacy database should be detected as version 0");
    Check(legacy.MigrationRequired(), "legacy database should require explicit migration");

    Check(legacy.GetAll(&records, &error) && records.size() == 1,
          "legacy records should remain readable before migration");
    Check(records.size() == 1 && records[0].title == "Registo legado",
          "legacy record should remain unchanged before migration");

    biblioled::LoanRecord blocked_write;
    blocked_write.title = "Alteração não autorizada";
    blocked_write.loan_date = "2024-01-16 10:30";
    blocked_write.type = "livro";
    Check(!legacy.Insert(&blocked_write, &error),
          "legacy database should reject writes before migration confirmation");
    Check(legacy.GetAll(&records, &error) && records.size() == 1,
          "rejected legacy write should leave records unchanged");

    std::string migration_backup;
    Check(legacy.MigrateLegacySchema(&migration_backup, &error),
          "confirmed legacy migration should succeed");
    Check(legacy.SchemaVersion() == 1, "migration should set schema version 1");
    Check(!legacy.MigrationRequired(), "successful migration should clear read-only state");
    Check(!legacy.HasSessionBackup(),
          "a safety backup for schema migration must not count as a daily loan-data backup");
    backups.push_back(migration_backup);

    Check(legacy.GetAll(&records, &error) && records.size() == 1,
          "migration should preserve every legacy loan record");
    Check(legacy.GetAuditEvents(&events, &error) && events.size() == 1,
          "migration should create one internal audit event");
    if (events.size() == 1) {
        Check(events[0].action == "schema_migration",
              "migration audit event should identify the migration");
        Check(events[0].details == "schema_version: 0 -> 1",
              "migration audit event should record source and target versions");
    }

    std::string migration_backup_decoded;
    Check(MakeReadableBackup(migration_backup, &migration_backup_decoded),
          "pre-migration backup should decompress for inspection");
    biblioled::LoanStore legacy_backup_reader;
    Check(legacy_backup_reader.Open(migration_backup_decoded, ".", &error),
          "decompressed pre-migration backup should remain openable");
    if (legacy_backup_reader.IsOpen()) {
        Check(legacy_backup_reader.SchemaVersion() == 0 &&
              legacy_backup_reader.MigrationRequired(),
              "pre-migration backup should remain at legacy schema version 0");
        Check(legacy_backup_reader.GetAll(&records, &error) && records.size() == 1,
              "pre-migration backup should preserve all legacy records");
        legacy_backup_reader.Close();
    }
    if (migration_backup_decoded != migration_backup) RemoveDatabaseFiles(migration_backup_decoded);
    legacy.Close();

    // A DDL failure after part of the schema has been created must roll
    // the whole migration back and leave the legacy database usable.
    Check(CreateLegacyDatabase(failed_ddl_migration_database, 0),
          "legacy database for mid-migration failure injection should be created");
    Check(MakeDirectory(failed_ddl_migration_backup_dir),
          "mid-migration failure backup directory should be created");

    sqlite3* conflict_db = NULL;
    const int conflict_open_rc = sqlite3_open(
        failed_ddl_migration_database.c_str(), &conflict_db);
    Check(conflict_open_rc == SQLITE_OK,
          "legacy database should open for migration-index conflict setup");
    if (conflict_open_rc == SQLITE_OK && conflict_db) {
        Check(Execute(conflict_db,
            "CREATE TABLE migration_conflict (id INTEGER PRIMARY KEY);"
            "CREATE INDEX idx_audit_events_occurred_at ON migration_conflict(id);"),
            "conflicting index should be created before migration");
        const int conflict_close_rc = sqlite3_close(conflict_db);
        Check(conflict_close_rc == SQLITE_OK,
              "migration-index conflict setup connection should close cleanly");
    } else if (conflict_db) {
        sqlite3_close(conflict_db);
    }

    biblioled::LoanStore failed_ddl_migration;
    Check(failed_ddl_migration.Open(
              failed_ddl_migration_database,
              failed_ddl_migration_backup_dir, &error),
          "legacy database with an unrelated index conflict should open");
    if (failed_ddl_migration.IsOpen()) {
        std::string failed_ddl_backup;
        error.clear();
        Check(!failed_ddl_migration.MigrateLegacySchema(&failed_ddl_backup, &error),
              "migration should fail when audit-index creation collides with an existing index");
        Check(!failed_ddl_backup.empty() && FileExists(failed_ddl_backup),
              "failed mid-migration attempt should retain its mandatory pre-migration backup");
        if (!failed_ddl_backup.empty()) backups.push_back(failed_ddl_backup);
        Check(failed_ddl_migration.IsOpen(),
              "a successful rollback should keep the legacy database connection usable");
        Check(failed_ddl_migration.SchemaVersion() == 0 &&
              failed_ddl_migration.MigrationRequired(),
              "failed DDL migration should preserve the version-0 read-only state");
        Check(failed_ddl_migration.GetAll(&records, &error) &&
              records.size() == 1 && records[0].title == "Registo legado",
              "failed DDL migration should preserve all legacy loan data");
        failed_ddl_migration.Close();
    }

    // Reopening runs the schema validator again. It must see an ordinary v0
    // database, not a partially created audit table left by the failed DDL.
    biblioled::LoanStore verified_failed_ddl_migration;
    Check(verified_failed_ddl_migration.Open(
              failed_ddl_migration_database,
              failed_ddl_migration_backup_dir, &error),
          "failed DDL migration should reopen as a clean legacy database");
    if (verified_failed_ddl_migration.IsOpen()) {
        Check(verified_failed_ddl_migration.SchemaVersion() == 0 &&
              verified_failed_ddl_migration.MigrationRequired(),
              "reopened failed DDL migration should still report schema version 0");
        Check(verified_failed_ddl_migration.GetAll(&records, &error) &&
              records.size() == 1 && records[0].title == "Registo legado",
              "reopened failed DDL migration should retain the original loan record");
        verified_failed_ddl_migration.Close();
    }
    RemoveFilesWithPrefix(failed_ddl_migration_backup_dir, "BiblioLED_");
#ifdef _WIN32
    RemoveDirectoryA(failed_ddl_migration_backup_dir.c_str());
#else
    rmdir(failed_ddl_migration_backup_dir.c_str());
#endif

    // Failed migration backup must leave the legacy database unchanged and readable.
    Check(CreateLegacyDatabase(failed_migration_database, 0),
          "second legacy database should be created");
    biblioled::LoanStore failed_migration;
    Check(failed_migration.Open(failed_migration_database, missing_backup_directory, &error),
          "legacy database should open even when backup directory is unavailable");
    if (failed_migration.IsOpen()) {
        Check(!failed_migration.MigrateLegacySchema(NULL, &error),
              "migration should fail if its mandatory backup cannot be created");
        Check(failed_migration.SchemaVersion() == 0 && failed_migration.MigrationRequired(),
              "failed migration should leave schema version 0 and read-only state intact");
        Check(failed_migration.GetAll(&records, &error) && records.size() == 1,
              "failed migration should preserve legacy loan records");
        failed_migration.Close();
    }

    // Unknown future versions and inconsistent version-1 databases are rejected.
    Check(CreateLegacyDatabase(unsupported_database, 7),
          "unsupported-version database should be created");
    biblioled::LoanStore unsupported;
    error.clear();
    Check(!unsupported.Open(unsupported_database, ".", &error),
          "application should reject a database schema newer than it supports");
    Check(!unsupported.IsOpen(),
          "a database with a future schema version must not remain open");
    Check(error.find("not supported") != std::string::npos &&
              error.find("not been modified") != std::string::npos,
          "future-schema rejection should explain that the database was not modified");
    Check(VerifyFutureVersionDatabaseUnchanged(unsupported_database),
          "future-schema rejection must preserve user_version and the existing loan row");
    unsupported.Close();

    Check(CreateLegacyDatabase(malformed_v1_database, 1),
          "malformed version-1 database should be created");
    biblioled::LoanStore malformed;
    Check(!malformed.Open(malformed_v1_database, ".", &error),
          "version 1 without its audit table should be rejected");
    malformed.Close();

    // Version 1 also requires the exact audit-table contract. Each malformed
    // variant keeps its loan row intact and must be refused before any write.
    const char* malformed_audit_type_sql =
        "CREATE TABLE audit_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, occurred_at TEXT NOT NULL, "
        "action INTEGER NOT NULL, loan_id INTEGER, before_title TEXT, "
        "before_loan_date TEXT, before_type TEXT, after_title TEXT, "
        "after_loan_date TEXT, after_type TEXT, details TEXT NOT NULL);";
    const char* malformed_audit_nullable_sql =
        "CREATE TABLE audit_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, occurred_at TEXT NOT NULL, "
        "action TEXT NOT NULL, loan_id INTEGER, before_title TEXT, "
        "before_loan_date TEXT, before_type TEXT, after_title TEXT, "
        "after_loan_date TEXT, after_type TEXT, details TEXT);";
    const char* malformed_audit_extra_sql =
        "CREATE TABLE audit_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, occurred_at TEXT NOT NULL, "
        "action TEXT NOT NULL, loan_id INTEGER, before_title TEXT, "
        "before_loan_date TEXT, before_type TEXT, after_title TEXT, "
        "after_loan_date TEXT, after_type TEXT, details TEXT NOT NULL, "
        "unsupported TEXT);";
    Check(CreateDatabaseWithAuditSchema(malformed_audit_type_database,
                                        malformed_audit_type_sql),
          "version-1 database with an incompatible audit action type should be created");
    Check(CreateDatabaseWithAuditSchema(malformed_audit_nullable_database,
                                        malformed_audit_nullable_sql),
          "version-1 database with nullable audit details should be created");
    Check(CreateDatabaseWithAuditSchema(malformed_audit_extra_database,
                                        malformed_audit_extra_sql),
          "version-1 database with an unsupported audit column should be created");
    const std::string malformed_audit_paths[] = {
        malformed_audit_type_database,
        malformed_audit_nullable_database,
        malformed_audit_extra_database
    };
    for (size_t i = 0; i < sizeof(malformed_audit_paths) / sizeof(malformed_audit_paths[0]); ++i) {
        biblioled::LoanStore incompatible_audit;
        Check(!incompatible_audit.Open(malformed_audit_paths[i], ".", &error),
              "version-1 database with an incompatible audit-table shape must be rejected");
        incompatible_audit.Close();
        Check(HasLegacyLoanTitle(malformed_audit_paths[i]),
              "rejecting a malformed version-1 audit table must preserve the loan row");
    }

    // A matching set of column names is not enough: the legacy data
    // contract requires an INTEGER primary key, TEXT NOT NULL fields, and
    // exactly the four supported columns.
    Check(CreateDatabaseWithSchema(
              malformed_legacy_type_database,
              "CREATE TABLE loans (id TEXT PRIMARY KEY, title TEXT NOT NULL, "
              "loan_date TEXT NOT NULL, type TEXT NOT NULL);"),
          "legacy database with an incompatible ID type should be created");
    Check(CreateDatabaseWithSchema(
              malformed_legacy_key_database,
              "CREATE TABLE loans (id INTEGER, title TEXT NOT NULL, "
              "loan_date TEXT NOT NULL, type TEXT NOT NULL);"),
          "legacy database without an ID primary key should be created");
    Check(CreateDatabaseWithSchema(
              malformed_legacy_nullable_database,
              "CREATE TABLE loans (id INTEGER PRIMARY KEY AUTOINCREMENT, "
              "title TEXT, loan_date TEXT NOT NULL, type TEXT NOT NULL);"),
          "legacy database with a nullable title column should be created");
    Check(CreateDatabaseWithSchema(
              malformed_legacy_extra_database,
              "CREATE TABLE loans (id INTEGER PRIMARY KEY AUTOINCREMENT, "
              "title TEXT NOT NULL, loan_date TEXT NOT NULL, type TEXT NOT NULL, "
              "unsupported TEXT NOT NULL DEFAULT '');"),
          "legacy database with an unsupported extra column should be created");

    const std::string malformed_legacy_paths[] = {
        malformed_legacy_type_database,
        malformed_legacy_key_database,
        malformed_legacy_nullable_database,
        malformed_legacy_extra_database
    };
    for (size_t i = 0; i < sizeof(malformed_legacy_paths) / sizeof(malformed_legacy_paths[0]); ++i) {
        biblioled::LoanStore incompatible;
        Check(!incompatible.Open(malformed_legacy_paths[i], ".", &error),
              "database with a look-alike but incompatible loans schema must be rejected");
        incompatible.Close();
        Check(HasLegacyLoanTitle(malformed_legacy_paths[i]),
              "schema rejection must leave the legacy record unchanged and readable");
    }

    // Backup-write failure still blocks mutations on current-schema databases.
    biblioled::LoanStore blocked;
    Check(blocked.Open(blocked_database, missing_backup_directory, &error),
          "database with a deliberately missing backup directory should open");
    if (blocked.IsOpen()) {
        biblioled::LoanRecord forbidden;
        forbidden.title = "Must not be inserted";
        forbidden.loan_date = "2099-02-01 10:00";
        forbidden.type = "livro";
        Check(!blocked.Insert(&forbidden, &error),
              "a failed session backup must block the database mutation");
        Check(!error.empty(), "backup failure should report an actionable error");
        Check(ReadAll(blocked, &records, "database should remain readable after backup failure"),
              "read after failed backup should succeed");
        Check(records.empty(), "failed backup must leave the database unchanged");
        blocked.Close();
    }

    // Offline recovery can rebuild a missing database and preserve an unusable
    // database plus any SQLite sidecars without touching the selected backup.
    const std::string recovery_source_database = database + ".recovery-source.sqlite";
    const std::string recovery_target_database = database + ".recovery-target.sqlite";
    const std::string corrupt_target_database = database + ".recovery-corrupt.sqlite";
    const std::string recovery_backup_directory = database + ".recovery-backups";
    const std::string locked_recovery_database = database + ".recovery-locked.sqlite";
    Check(MakeDirectory(recovery_backup_directory),
          "isolated recovery backup directory should be created");

    biblioled::LoanStore recovery_source;
    Check(recovery_source.Open(recovery_source_database, recovery_backup_directory, &error),
          "recovery source database should open");
    std::string recovery_backup;
    if (recovery_source.IsOpen()) {
        biblioled::LoanRecord recovery_record;
        recovery_record.title = "Registo para recuperação";
        recovery_record.loan_date = "2099-03-01 10:00";
        recovery_record.type = "livro";
        Check(recovery_source.Insert(&recovery_record, &error),
              "recovery source record should be inserted");
        Check(recovery_source.CreateManualBackup(&recovery_backup, &error),
              "a validated manual backup should be created for recovery tests");
        recovery_source.Close();
    }

    // A decompression target must not reuse a basename that has an orphaned
    // SQLite WAL from an earlier interrupted restore/recovery.
    const std::string stale_decoded_wal =
        recovery_backup + ".decoded_sqlite_0.tmp-wal";
    {
        std::ofstream stale_wal(stale_decoded_wal.c_str(),
                                std::ios::out | std::ios::binary);
        stale_wal << "stale WAL from an interrupted decompression";
    }
    Check(FileExists(stale_decoded_wal),
          "stale decompression WAL sidecar should be created");

    // An abandoned sidecar from a previous interrupted recovery must not
    // be attached to a newly created staging database.
    const std::string stale_staging_wal =
        recovery_target_database + ".recovery_staging_0.tmp-wal";
    {
        std::ofstream stale_wal(stale_staging_wal.c_str(),
                                std::ios::out | std::ios::binary);
        stale_wal << "stale sidecar from an interrupted recovery";
    }
    Check(FileExists(stale_staging_wal),
          "stale recovery staging sidecar should be created");

    biblioled::LoanStore missing_recovery;
    std::string preserved_database;
    Check(missing_recovery.RecoverFromBackup(
              recovery_target_database, recovery_backup, &preserved_database, &error),
          "a valid backup should recover a missing database");
    Check(FileExists(recovery_target_database),
          "missing-database recovery should install the restored database");
    Check(FileExists(stale_decoded_wal),
          "backup decoding should leave an unrelated stale WAL sidecar untouched");
    Check(!FileExists(recovery_backup + ".decoded_sqlite_1.tmp"),
          "decoded SQLite temporary file should be removed after recovery");
    Check(FileExists(stale_staging_wal),
          "recovery should leave an unrelated stale staging sidecar untouched");
    Check(preserved_database.empty(),
          "missing-database recovery should not report a preserved database that did not exist");
    biblioled::LoanStore recovered_missing;
    Check(recovered_missing.Open(recovery_target_database, recovery_backup_directory, &error),
          "recovered missing database should open normally");
    if (recovered_missing.IsOpen()) {
        Check(recovered_missing.GetAll(&records, &error) && records.size() == 1 &&
              records[0].title == "Registo para recuperação",
              "recovered missing database should contain the backup's records");
        recovered_missing.Close();
    }

    {
        std::ofstream corrupt_output(corrupt_target_database.c_str(),
                                     std::ios::out | std::ios::binary);
        corrupt_output << "not a SQLite database";
    }
    {
        std::ofstream journal_output((corrupt_target_database + "-journal").c_str(),
                                     std::ios::out | std::ios::binary);
        journal_output << "unrecognised stale journal";
    }

    biblioled::LoanStore invalid_recovery;
    Check(!invalid_recovery.RecoverFromBackup(
              corrupt_target_database, invalid_backup, NULL, &error),
          "offline recovery should reject an invalid backup");
    Check(FileExists(corrupt_target_database) &&
          FileExists(corrupt_target_database + "-journal"),
          "an invalid recovery source must leave the damaged database and sidecar untouched");

#ifdef _WIN32
    // Hold a sidecar without FILE_SHARE_DELETE so preservation fails after the
    // main file has moved. Recovery must roll that first move back and leave the
    // expected database and all original bytes in place.
    const std::string locked_sidecar = locked_recovery_database + "-journal";
    {
        std::ofstream locked_db_output(
            locked_recovery_database.c_str(), std::ios::out | std::ios::binary);
        locked_db_output << "original corrupt database bytes";
    }
    {
        std::ofstream locked_sidecar_output(
            locked_sidecar.c_str(), std::ios::out | std::ios::binary);
        locked_sidecar_output << "locked original sidecar bytes";
    }

    HANDLE locked_sidecar_handle = CreateFileA(
        locked_sidecar.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    Check(locked_sidecar_handle != INVALID_HANDLE_VALUE,
          "recovery failure-injection sidecar should open without sharing");
    if (locked_sidecar_handle != INVALID_HANDLE_VALUE) {
        biblioled::LoanStore locked_recovery;
        std::string locked_preserved;
        Check(!locked_recovery.RecoverFromBackup(
                  locked_recovery_database, recovery_backup,
                  &locked_preserved, &error),
              "recovery should fail when an original sidecar cannot be moved");
        const DWORD locked_sidecar_attributes =
            GetFileAttributesA(locked_sidecar.c_str());
        Check(FileExists(locked_recovery_database) &&
              locked_sidecar_attributes != INVALID_FILE_ATTRIBUTES &&
              (locked_sidecar_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0,
              "failed sidecar preservation should roll the original database and sidecar back");
        Check(locked_preserved.empty(),
              "failed recovery should not report a completed preservation group");
        CloseHandle(locked_sidecar_handle);
        Check(FileExists(locked_sidecar),
              "the original locked sidecar should remain after the lock is released");
    }
    {
        std::ifstream original_input(
            locked_recovery_database.c_str(), std::ios::in | std::ios::binary);
        std::string original_bytes;
        if (original_input.good()) {
            std::getline(original_input, original_bytes);
        }
        Check(original_bytes == "original corrupt database bytes",
              "failed recovery rollback should preserve the original database bytes");
    }
    Check(!HasFileWithPrefix(".", locked_recovery_database + ".recovery_preserved_"),
          "successful rollback should not leave a partially preserved original database");
    RemoveDatabaseFiles(locked_recovery_database);
#endif

    biblioled::LoanStore corrupt_recovery;
    preserved_database.clear();
    Check(corrupt_recovery.RecoverFromBackup(
              corrupt_target_database, recovery_backup, &preserved_database, &error),
          "a valid backup should recover a corrupt database");
    Check(!preserved_database.empty() && FileExists(preserved_database),
          "the original corrupt database should be preserved under a recovery filename");
    Check(FileExists(preserved_database + "-journal"),
          "the original SQLite journal sidecar should be preserved with the corrupt database");
    biblioled::LoanStore recovered_corrupt;
    Check(recovered_corrupt.Open(corrupt_target_database, recovery_backup_directory, &error),
          "recovered corrupt database should open normally");
    if (recovered_corrupt.IsOpen()) {
        Check(recovered_corrupt.GetAll(&records, &error) && records.size() == 1 &&
              records[0].title == "Registo para recuperação",
              "recovered corrupt database should contain the selected backup's records");
        recovered_corrupt.Close();
    }

    // A failed validation must not consume or modify the user's selected backup.
    Check(FileExists(recovery_backup),
          "successful recovery should leave the selected backup available");

    RemoveDatabaseFiles(recovery_backup + ".decoded_sqlite_0.tmp");
    RemoveDatabaseFiles(recovery_backup + ".decoded_sqlite_1.tmp");
    RemoveDatabaseFiles(recovery_source_database);
    RemoveDatabaseFiles(recovery_target_database);
    RemoveDatabaseFiles(recovery_target_database + ".recovery_staging_0.tmp");
    RemoveDatabaseFiles(corrupt_target_database);
    RemoveDatabaseFiles(locked_recovery_database);
    RemoveDatabaseFiles(preserved_database);
    RemoveFilesWithPrefix(recovery_backup_directory, "BiblioLED_");
#ifdef _WIN32
    RemoveDirectoryA(recovery_backup_directory.c_str());
#else
    rmdir(recovery_backup_directory.c_str());
#endif

    RemoveDatabaseFiles(database);
    RemoveDatabaseFiles(blocked_database);
    RemoveDatabaseFiles(legacy_database);
    RemoveDatabaseFiles(failed_migration_database);
    RemoveDatabaseFiles(failed_ddl_migration_database);
    RemoveDatabaseFiles(unsupported_database);
    RemoveDatabaseFiles(malformed_v1_database);
    RemoveDatabaseFiles(malformed_audit_type_database);
    RemoveDatabaseFiles(malformed_audit_nullable_database);
    RemoveDatabaseFiles(malformed_audit_extra_database);
    RemoveDatabaseFiles(malformed_legacy_type_database);
    RemoveDatabaseFiles(malformed_legacy_key_database);
    RemoveDatabaseFiles(malformed_legacy_nullable_database);
    RemoveDatabaseFiles(malformed_legacy_extra_database);
    RemoveDatabaseFiles(invalid_backup);
    for (size_t i = 0; i < backups.size(); ++i) {
        RemoveDatabaseFiles(backups[i]);
    }

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed." << std::endl;
        return 1;
    }

    std::cout << "LoanStore tests passed." << std::endl;
    return 0;
}
