#ifndef BIBLIOLED_LOAN_RULES_H
#define BIBLIOLED_LOAN_RULES_H

#include <ctime>
#include <string>

namespace biblioled {

// The XP-targeted 32-bit C runtime has a 32-bit std::time_t. Keep persisted
// local loan dates in a 64-bit timestamp so valid four-digit years do not
// inherit the runtime's 2038 limit.
typedef long long LoanTimestamp;

struct LoanEvaluation {
    LoanTimestamp loan_time;
    LoanTimestamp expiry_time;
    long days_remaining;
    bool future_dated;
    bool expired;
    bool visible_in_operational_list;

    LoanEvaluation()
        : loan_time(static_cast<LoanTimestamp>(-1)),
          expiry_time(static_cast<LoanTimestamp>(-1)),
          days_remaining(0),
          future_dated(false),
          expired(false),
          visible_in_operational_list(false)
    {
    }
};

struct QuotaWindowBounds {
    std::string daily_since;
    std::string monthly_since;
};

struct QuotaAvailability {
    int daily_remaining;
    int monthly_remaining;

    QuotaAvailability() : daily_remaining(0), monthly_remaining(0) {}
};

LoanTimestamp ParseLoanDateTime(const std::string& value);
std::string FormatLocalDateTime(LoanTimestamp value, const char* format);
bool GetLocalDateTimeParts(LoanTimestamp value, std::tm* parts);
LoanTimestamp AddLocalCalendarDays(LoanTimestamp value, int days);

bool EvaluateLoanDateTime(const std::string& loan_date,
                          int duration_days,
                          LoanTimestamp now,
                          LoanEvaluation* evaluation);

bool BuildQuotaWindowBounds(LoanTimestamp now,
                            int monthly_window_days,
                            QuotaWindowBounds* bounds);

bool CalculateQuotaAvailability(int daily_limit,
                                int daily_count,
                                int monthly_limit,
                                int monthly_count,
                                QuotaAvailability* availability);

} // namespace biblioled

#endif
