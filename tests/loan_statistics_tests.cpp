#include "biblioled/loan_statistics.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

biblioled::LoanRecord MakeRecord(const char* date,
                                 const char* type)
{
    biblioled::LoanRecord record;
    record.loan_date = date;
    record.type = type;
    return record;
}

const biblioled::AnnualLoanStatistics* FindYear(
    const biblioled::LoanStatistics& statistics, int year)
{
    for (size_t i = 0; i < statistics.annual.size(); ++i) {
        if (statistics.annual[i].year == year) return &statistics.annual[i];
    }
    return NULL;
}

void TestGeneralAndAnnualStatistics()
{
    std::vector<biblioled::LoanRecord> records;
    records.push_back(MakeRecord("2023-12-31 23:50", "livro"));
    records.push_back(MakeRecord("2024-01-01 08:00", "audiolivro"));
    records.push_back(MakeRecord("2024-01-01 09:00", "livro"));
    records.push_back(MakeRecord("2024-01-03 18:30", "audiolivro"));
    records.push_back(MakeRecord("2024-03-10 19:00", "livro"));
    records.push_back(MakeRecord("not-a-date", "revista"));

    biblioled::LoanStatistics statistics;
    Check(biblioled::CalculateLoanStatistics(records, 0, &statistics),
          "statistics should be calculated");

    Check(statistics.total_records == 6 &&
          statistics.valid_date_records == 5 &&
          statistics.invalid_date_records == 1,
          "all records and invalid dates should be accounted for");
    Check(statistics.book_count == 3 &&
          statistics.audiobook_count == 2 &&
          statistics.other_type_count == 1,
          "type totals should include unknown types separately");
    Check(statistics.first_loan_date == "2023-12-31" &&
          statistics.last_loan_date == "2024-03-10",
          "first and last valid loan dates should be identified");
    Check(statistics.selected_year == 2024 &&
          statistics.selected_year_count == 4 &&
          statistics.previous_year_count == 1 &&
          statistics.year_difference == 3,
          "latest year should be selected and compared with the previous year");

    const biblioled::AnnualLoanStatistics* year2024 = FindYear(statistics, 2024);
    Check(year2024 != NULL, "annual statistics should include 2024");
    if (year2024) {
        Check(year2024->loan_count == 4 &&
              year2024->previous_year_count == 1 &&
              year2024->difference == 3,
              "annual evolution should contain year-over-year difference");
    }

    Check(statistics.daily_activity.size() == 3,
          "selected-year activity should include each active calendar day");
    if (statistics.daily_activity.size() == 3) {
        Check(statistics.daily_activity[0].date == "2024-01-01" &&
              statistics.daily_activity[0].loan_count == 2,
              "same-day loans should be grouped in the activity calendar");
    }
}

void TestUsagePatternsAndIntervals()
{
    std::vector<biblioled::LoanRecord> records;
    records.push_back(MakeRecord("2023-12-31 23:50", "livro"));
    records.push_back(MakeRecord("2024-01-01 08:00", "audiolivro"));
    records.push_back(MakeRecord("2024-01-01 09:00", "livro"));
    records.push_back(MakeRecord("2024-01-03 18:30", "audiolivro"));
    records.push_back(MakeRecord("2024-03-10 19:00", "livro"));

    biblioled::LoanStatistics statistics;
    Check(biblioled::CalculateLoanStatistics(records, 2024, &statistics),
          "selected-year statistics should be calculated");
    Check(statistics.weekday_counts[0] == 2 &&
          statistics.weekday_counts[2] == 1 &&
          statistics.weekday_counts[6] == 1,
          "weekday distribution should be Monday-first and scoped to the selected year");
    Check(statistics.hour_counts[8] == 1 &&
          statistics.hour_counts[9] == 1 &&
          statistics.hour_counts[18] == 1 &&
          statistics.hour_counts[19] == 1,
          "hour distribution should count local loan hours");
    Check(statistics.interval_count == 3,
          "intervals should be measured between distinct active days");
    Check(std::fabs(statistics.mean_days_between - (70.0 / 3.0)) < 0.0001,
          "mean interval should use calendar-day differences");
    Check(std::fabs(statistics.median_days_between - 2.0) < 0.0001,
          "median interval should use the middle sorted interval");

    biblioled::LoanStatistics previous_year;
    Check(biblioled::CalculateLoanStatistics(records, 2023, &previous_year),
          "previous-year statistics should be available");
    Check(previous_year.selected_year_count == 1 &&
          previous_year.daily_activity.size() == 1 &&
          previous_year.weekday_counts[6] == 1,
          "year selection should filter the activity calendar and patterns");
    Check(previous_year.interval_count == 3 &&
          std::fabs(previous_year.mean_days_between - (70.0 / 3.0)) < 0.0001,
          "mean/median intervals should remain scoped to the full history, not the selected year");
}

void TestNoActivityAndSingleDay()
{
    std::vector<biblioled::LoanRecord> records;
    biblioled::LoanStatistics empty;
    Check(biblioled::CalculateLoanStatistics(records, 0, &empty),
          "empty history should produce valid zero statistics");
    Check(empty.total_records == 0 && empty.annual.empty() &&
          empty.interval_count == 0 && empty.mean_days_between == 0.0 &&
          empty.median_days_between == 0.0,
          "empty history should yield zero statistics");

    records.push_back(MakeRecord("2024-02-01 08:00", "livro"));
    records.push_back(MakeRecord("2024-02-01 15:00", "livro"));
    biblioled::LoanStatistics one_day;
    Check(biblioled::CalculateLoanStatistics(records, 2024, &one_day),
          "single active day should be calculated");
    Check(one_day.daily_activity.size() == 1 &&
          one_day.daily_activity[0].loan_count == 2,
          "same-day records should share one calendar cell");
    Check(one_day.interval_count == 0 &&
          one_day.mean_days_between == 0.0 &&
          one_day.median_days_between == 0.0,
          "one active day should have no inter-day interval");
}

void TestNullOutput()
{
    std::vector<biblioled::LoanRecord> records;
    Check(!biblioled::CalculateLoanStatistics(records, 0, NULL),
          "null statistics output should be rejected");
}

} // namespace

int main()
{
    TestGeneralAndAnnualStatistics();
    TestUsagePatternsAndIntervals();
    TestNoActivityAndSingleDay();
    TestNullOutput();

    if (failures != 0) {
        std::cerr << failures << " loan-statistics test(s) failed." << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "All loan-statistics tests passed." << std::endl;
    return EXIT_SUCCESS;
}
