#ifndef BIBLIOLED_LOAN_STATISTICS_H
#define BIBLIOLED_LOAN_STATISTICS_H

#include "biblioled/loan_store.h"

#include <cstddef>
#include <string>
#include <vector>

namespace biblioled {

struct AnnualLoanStatistics {
    int year;
    int loan_count;
    int previous_year_count;
    int difference;

    AnnualLoanStatistics()
        : year(0), loan_count(0), previous_year_count(0), difference(0) {}
};

struct DailyLoanActivity {
    std::string date;
    int loan_count;
};

struct LoanStatistics {
    std::size_t total_records;
    std::size_t valid_date_records;
    std::size_t invalid_date_records;
    int book_count;
    int audiobook_count;
    int other_type_count;
    std::string first_loan_date;
    std::string last_loan_date;
    int selected_year;
    int selected_year_count;
    int previous_year_count;
    int year_difference;
    std::vector<AnnualLoanStatistics> annual;
    std::vector<DailyLoanActivity> daily_activity;
    int weekday_counts[7]; // Monday = 0, Sunday = 6.
    int hour_counts[24];
    std::size_t interval_count;
    double mean_days_between;
    double median_days_between;

    LoanStatistics();
};

// Statistics use all records for totals and annual evolution. Calendar,
// weekday and hour activity are scoped to selected_year. Mean/median intervals
// use distinct dates with at least one loan across the full history, matching
// the legacy mean metric; they are not scoped to selected_year.
bool CalculateLoanStatistics(const std::vector<LoanRecord>& records,
                             int selected_year,
                             LoanStatistics* statistics);

} // namespace biblioled

#endif
