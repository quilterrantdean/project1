#ifndef BIBLIOLED_AUDIT_LOG_H
#define BIBLIOLED_AUDIT_LOG_H
#include "biblioled/loan_store.h"
#include <string>
#include <vector>
namespace biblioled {
bool SynchronizeAuditLog(const std::wstring& directory,
                         const std::vector<AuditEvent>& events,
                         unsigned long maximum_file_size,
                         int* written_count,
                         std::string* error);
}
#endif
