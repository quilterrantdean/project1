#include "biblioled/loan_statistics.h"

#include "biblioled/loan_rules.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <map>
#include <set>
#include <stdint.h>
#include <ctime>

namespace biblioled {
namespace {

typedef long long CalendarDay;

CalendarDay DaysFromCivil(int year, unsigned int month, unsigned int day)
{
    long long adjusted_year = year;
    adjusted_year -= month <= 2 ? 1 : 0;

    const long long era = (adjusted_year >= 0
        ? adjusted_year : adjusted_year - 399) / 400;
    const unsigned int year_of_era =
        static_cast<unsigned int>(adjusted_year - era * 400);
    const unsigned int shifted_month =
        month > 2 ? month - 3 : month + 9;
    const unsigned int day_of_year =
        (153 * shifted_month + 2) / 5 + day - 1;
    const unsigned int day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
        day_of_year;
    return era * 146097 + static_cast<long long>(day_of_era) - 719468;
}

std::string LowerAscii(const std::string& value)
{
    std::string result(value);
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(result[i])));
    }
    return result;
}

} // namespace

LoanStatistics::LoanStatistics()
    : total_records(0),
      valid_date_records(0),
      invalid_date_records(0),
      book_count(0),
      audiobook_count(0),
      other_type_count(0),
      selected_year(0),
      selected_year_count(0),
      previous_year_count(0),
      year_difference(0),
      interval_count(0),
      mean_days_between(0.0),
      median_days_between(0.0)
{
    for (int i = 0; i < 7; ++i) weekday_counts[i] = 0;
    for (int i = 0; i < 24; ++i) hour_counts[i] = 0;
}

bool CalculateLoanStatistics(const std::vector<LoanRecord>& records,
                             int selected_year,
                             LoanStatistics* statistics)
{
    if (!statistics) return false;

    LoanStatistics result;
    result.total_records = records.size();

    std::map<int, int> annual_counts;
    std::map<std::string, int> daily_counts;
    std::set<CalendarDay> active_days;

    for (size_t i = 0; i < records.size(); ++i) {
        const std::string type = LowerAscii(records[i].type);
        if (type == "livro") ++result.book_count;
        else if (type == "audiolivro") ++result.audiobook_count;
        else ++result.other_type_count;

        const std::time_t timestamp = ParseLoanDateTime(records[i].loan_date);
        if (timestamp == static_cast<std::time_t>(-1)) {
            ++result.invalid_date_records;
            continue;
        }

        const std::tm* local_pointer = std::localtime(&timestamp);
        if (!local_pointer) {
            ++result.invalid_date_records;
            continue;
        }
        const std::tm local = *local_pointer;
        const int year = local.tm_year + 1900;
        const std::string date =
            FormatLocalDateTime(timestamp, "%Y-%m-%d");
        if (date.empty()) {
            ++result.invalid_date_records;
            continue;
        }

        ++result.valid_date_records;
        ++annual_counts[year];
        ++daily_counts[date];
        active_days.insert(DaysFromCivil(
            year, static_cast<unsigned int>(local.tm_mon + 1),
            static_cast<unsigned int>(local.tm_mday)));
    }

    if (result.valid_date_records > 0) {
        result.first_loan_date = daily_counts.begin()->first;
        result.last_loan_date = daily_counts.rbegin()->first;
    }

    for (std::map<int, int>::const_iterator it = annual_counts.begin();
         it != annual_counts.end(); ++it) {
        AnnualLoanStatistics item;
        item.year = it->first;
        item.loan_count = it->second;
        const std::map<int, int>::const_iterator previous =
            annual_counts.find(it->first - 1);
        item.previous_year_count = previous == annual_counts.end()
            ? 0 : previous->second;
        item.difference = item.loan_count - item.previous_year_count;
        result.annual.push_back(item);
    }

    if (selected_year == 0 && !annual_counts.empty()) {
        selected_year = annual_counts.rbegin()->first;
    }
    result.selected_year = selected_year;

    const std::map<int, int>::const_iterator selected =
        annual_counts.find(selected_year);
    if (selected != annual_counts.end()) {
        result.selected_year_count = selected->second;
    }
    const std::map<int, int>::const_iterator previous =
        annual_counts.find(selected_year - 1);
    if (previous != annual_counts.end()) {
        result.previous_year_count = previous->second;
    }
    result.year_difference =
        result.selected_year_count - result.previous_year_count;

    char selected_year_text[16] = {0};
    std::sprintf(selected_year_text, "%04d", selected_year);
    const std::string year_prefix(selected_year_text);

    for (std::map<std::string, int>::const_iterator it = daily_counts.begin();
         it != daily_counts.end(); ++it) {
        if (selected_year != 0 && it->first.compare(0, 4, year_prefix) == 0) {
            DailyLoanActivity item;
            item.date = it->first;
            item.loan_count = it->second;
            result.daily_activity.push_back(item);
        }
    }

    // Re-parse the valid dates only for the selected year's weekday/hour
    // distribution. This keeps the statistics independent of the GUI.
    for (size_t i = 0; i < records.size(); ++i) {
        const std::time_t timestamp = ParseLoanDateTime(records[i].loan_date);
        if (timestamp == static_cast<std::time_t>(-1)) continue;
        const std::tm* local_pointer = std::localtime(&timestamp);
        if (!local_pointer) continue;
        const std::tm local = *local_pointer;
        if (local.tm_year + 1900 != selected_year) continue;

        const int monday_first_weekday = (local.tm_wday + 6) % 7;
        if (monday_first_weekday >= 0 && monday_first_weekday < 7) {
            ++result.weekday_counts[monday_first_weekday];
        }
        if (local.tm_hour >= 0 && local.tm_hour < 24) {
            ++result.hour_counts[local.tm_hour];
        }
    }

    if (active_days.size() > 1) {
        std::vector<CalendarDay> intervals;
        intervals.reserve(active_days.size() - 1);
        std::set<CalendarDay>::const_iterator previous_day = active_days.begin();
        std::set<CalendarDay>::const_iterator current_day = previous_day;
        ++current_day;
        CalendarDay total_days = 0;
        for (; current_day != active_days.end(); ++current_day, ++previous_day) {
            const CalendarDay difference = *current_day - *previous_day;
            intervals.push_back(difference);
            total_days += difference;
        }

        std::sort(intervals.begin(), intervals.end());
        result.interval_count = intervals.size();
        result.mean_days_between =
            static_cast<double>(total_days) / intervals.size();

        const size_t middle = intervals.size() / 2;
        if (intervals.size() % 2 == 0) {
            result.median_days_between =
                (static_cast<double>(intervals[middle - 1]) +
                 static_cast<double>(intervals[middle])) / 2.0;
        } else {
            result.median_days_between = static_cast<double>(intervals[middle]);
        }
    }

    *statistics = result;
    return true;
}

} // namespace biblioled
