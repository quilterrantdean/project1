#ifndef BIBLIOLED_LOAN_STORE_H
#define BIBLIOLED_LOAN_STORE_H

#include <string>
#include <vector>

struct sqlite3;

namespace biblioled {

struct LoanRecord {
    int id;
    std::string title;
    std::string loan_date;
    std::string type;

    LoanRecord() : id(0) {}
};

struct AuditEvent {
    long long id;
    std::string occurred_at;
    std::string action;
    int loan_id;
    bool has_before;
    LoanRecord before;
    bool has_after;
    LoanRecord after;
    std::string details;

    AuditEvent() : id(0), loan_id(0), has_before(false), has_after(false) {}
};

class LoanStore {
public:
    LoanStore();
    ~LoanStore();

    LoanStore(const LoanStore&) = delete;
    LoanStore& operator=(const LoanStore&) = delete;

    bool Open(const std::string& database_path,
              const std::string& backup_directory,
              std::string* error);
    void Close();
    bool IsOpen() const;
    void SetBackupCompression(const std::string& format, int level);
    int SchemaVersion() const;
    bool MigrationRequired() const;
    bool MigrateLegacySchema(std::string* backup_path, std::string* error);

    bool CreateManualBackup(std::string* backup_path, std::string* error);
    bool RestoreFromBackup(const std::string& backup_path,
                           std::string* current_backup_path,
                           std::string* error);

    // Restore a validated backup while the expected database is closed or unavailable.
    // Any existing database and SQLite sidecars are retained under a recovery filename.
    bool RecoverFromBackup(const std::string& database_path,
                           const std::string& backup_path,
                           std::string* preserved_database_path,
                           std::string* error);

    bool HasSessionBackup() const;
    const std::string& LastSessionBackupPath() const;

    bool GetAll(std::vector<LoanRecord>* records, std::string* error);
    bool GetById(int id, LoanRecord* record, std::string* error);
    bool GetAuditEvents(std::vector<AuditEvent>* events, std::string* error);
    bool RecordAuditEvent(const std::string& action,
                          const std::string& details,
                          std::string* error);
    // Queue warnings/errors/failures in SQLite even when the external log is
    // unhealthy. This does not restore the log or unblock loan mutations.
    bool RecordDiagnosticEvent(const std::string& severity,
                               const std::string& details,
                               std::string* error);
    void SetExternalAuditLogStatus(bool enabled, bool healthy);
    bool ExternalAuditLogBlocked() const;
    bool Insert(LoanRecord* record, std::string* error);
    bool Update(const LoanRecord& record, std::string* error);
    bool Remove(int id, std::string* error);
    bool CountSince(const std::string& date_time, int* count, std::string* error);
    bool QuickCheck(std::string* error);

private:
    // Narrow test access permits deterministic injection of an SQLite ROLLBACK
    // authorizer failure; no runtime test hook or public database handle exists.
    friend class LoanStoreTestAccess;

    bool CreateInitialSchema(std::string* error);
    bool ValidateExistingSchema(std::string* error);
    bool EnsureAutomaticBackup(std::string* error);
    bool RollbackTransaction(std::string* error);
    bool RecordAuditEventInternal(const std::string& action,
                                  const std::string& details,
                                  bool allow_external_log_block,
                                  std::string* error);
    bool CreateAndValidateBackup(const std::string& path, std::string* error);
    std::string MakeBackupPath(const std::string& category) const;
    void SetError(std::string* error, const std::string& message) const;

    sqlite3* db_;
    std::string database_path_;
    std::string backup_directory_;
    std::string last_session_backup_path_;
    bool session_backup_done_;
    int schema_version_;
    bool migration_required_;
    bool external_audit_log_enabled_;
    bool external_audit_log_healthy_;
    std::string backup_compression_format_;
    int backup_compression_level_;
};

} // namespace biblioled

#endif
