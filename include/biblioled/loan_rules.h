#ifndef BIBLIOLED_LOAN_RULES_H
#define BIBLIOLED_LOAN_RULES_H

#include <ctime>
#include <string>

namespace biblioled {

struct LoanEvaluation {
    std::time_t loan_time;
    std::time_t expiry_time;
    long days_remaining;
    bool future_dated;
    bool expired;
    bool visible_in_operational_list;

    LoanEvaluation()
        : loan_time(static_cast<std::time_t>(-1)),
          expiry_time(static_cast<std::time_t>(-1)),
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

std::time_t ParseLoanDateTime(const std::string& value);
std::string FormatLocalDateTime(std::time_t value, const char* format);
std::time_t AddLocalCalendarDays(std::time_t value, int days);

bool EvaluateLoanDateTime(const std::string& loan_date,
                          int duration_days,
                          std::time_t now,
                          LoanEvaluation* evaluation);

bool BuildQuotaWindowBounds(std::time_t now,
                            int monthly_window_days,
                            QuotaWindowBounds* bounds);

bool CalculateQuotaAvailability(int daily_limit,
                                int daily_count,
                                int monthly_limit,
                                int monthly_count,
                                QuotaAvailability* availability);

} // namespace biblioled

#endif
