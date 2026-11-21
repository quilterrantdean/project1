#include "biblioled/csv_export.h"
namespace biblioled {
namespace {
std::string EscapeCsvField(const std::string& value)
{
    const bool quote = value.find(';') != std::string::npos ||
        value.find('"') != std::string::npos ||
        value.find('\r') != std::string::npos ||
        value.find('\n') != std::string::npos;
    if (!quote) return value;
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '"') escaped.push_back('"');
        escaped.push_back(value[i]);
    }
    escaped.push_back('"');
    return escaped;
}
}
std::string BuildLoanRecordsCsv(const std::vector<LoanRecord>& records)
{
    std::string output("\xEF\xBB\xBF", 3);
    output += "Title;LoanDate;Type\r\n";
    for (size_t i = 0; i < records.size(); ++i) {
        output += EscapeCsvField(records[i].title);
        output.push_back(';');
        output += EscapeCsvField(records[i].loan_date);
        output.push_back(';');
        output += EscapeCsvField(records[i].type);
        output += "\r\n";
    }
    return output;
}
}
