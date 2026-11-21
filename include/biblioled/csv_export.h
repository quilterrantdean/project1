#ifndef BIBLIOLED_CSV_EXPORT_H
#define BIBLIOLED_CSV_EXPORT_H
#include "biblioled/loan_store.h"
#include <string>
#include <vector>
namespace biblioled {
// UTF-8 BOM, semicolon delimiter, CRLF and the legacy-compatible columns.
// Stored values are exported unchanged, including future and invalid legacy dates.
std::string BuildLoanRecordsCsv(const std::vector<LoanRecord>& records);
}
#endif
