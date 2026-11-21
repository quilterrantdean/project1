#include "biblioled/loan_rules.h"

#include <ctime>
#include <climits>

namespace biblioled {

namespace {

long long DaysFromCivil(const std::tm& value)
{
    long long year = static_cast<long long>(value.tm_year) + 1900;
    const unsigned int month = static_cast<unsigned int>(value.tm_mon + 1);
    const unsigned int day = static_cast<unsigned int>(value.tm_mday);
    year -= month <= 2 ? 1 : 0;

    const long long era =
        (year >= 0 ? year : year - 399) / 400;
    const unsigned int year_of_era =
        static_cast<unsigned int>(year - era * 400);
    const unsigned int shifted_month =
        month > 2 ? month - 3 : month + 9;
    const unsigned int day_of_year =
        (153 * shifted_month + 2) / 5 + day - 1;
    const unsigned int day_of_era =
        year_of_era * 365 + year_of_era / 4 -
        year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<long long>(day_of_era) - 719468;
}

bool CalculateLocalDaysRemaining(std::time_t expiry,
                                 std::time_t now,
                                 long* days_remaining)
{
    if (!days_remaining) return false;

    const std::tm* expiry_value = std::localtime(&expiry);
    if (!expiry_value) return false;
    const std::tm expiry_parts = *expiry_value;

    const std::tm* now_value = std::localtime(&now);
    if (!now_value) return false;
    const std::tm now_parts = *now_value;

    const long long expiry_seconds =
        expiry_parts.tm_hour * 3600LL +
        expiry_parts.tm_min * 60LL + expiry_parts.tm_sec;
    const long long now_seconds =
        now_parts.tm_hour * 3600LL +
        now_parts.tm_min * 60LL + now_parts.tm_sec;
    const long long wall_clock_difference =
        (DaysFromCivil(expiry_parts) - DaysFromCivil(now_parts)) * 86400LL +
        expiry_seconds - now_seconds;

    long long whole_days = wall_clock_difference / 86400LL;
    if (wall_clock_difference < 0 &&
        wall_clock_difference % 86400LL != 0) {
        --whole_days;
    }

    if (whole_days < LONG_MIN || whole_days > LONG_MAX) return false;
    *days_remaining = static_cast<long>(whole_days);
    return true;
}

} // namespace

std::time_t ParseLoanDateTime(const std::string& value)
{
    // Persisted loan dates use one fixed, locale-independent representation:
    // YYYY-MM-DD HH:MM. Do not let scanf accept signs, whitespace, or
    // variable-width fields that would create non-canonical stored values.
    if (value.size() != 16 || value[4] != '-' || value[7] != '-' ||
        value[10] != ' ' || value[13] != ':') {
        return static_cast<std::time_t>(-1);
    }

    int digits[12] = {0};
    int digit_index = 0;
    for (std::string::size_type i = 0; i < value.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13) continue;
        if (value[i] < '0' || value[i] > '9') {
            return static_cast<std::time_t>(-1);
        }
        digits[digit_index++] = value[i] - '0';
    }

    const int year = digits[0] * 1000 + digits[1] * 100 +
                     digits[2] * 10 + digits[3];
    const int month = digits[4] * 10 + digits[5];
    const int day = digits[6] * 10 + digits[7];
    const int hour = digits[8] * 10 + digits[9];
    const int minute = digits[10] * 10 + digits[11];

    if (year < 1900 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59) {
        return static_cast<std::time_t>(-1);
    }

    std::tm parts = {};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_isdst = -1;

    const std::time_t result = std::mktime(&parts);
    if (result == static_cast<std::time_t>(-1)) return result;

    const std::tm* checked = std::localtime(&result);
    if (!checked ||
        checked->tm_year != year - 1900 ||
        checked->tm_mon != month - 1 ||
        checked->tm_mday != day ||
        checked->tm_hour != hour ||
        checked->tm_min != minute) {
        return static_cast<std::time_t>(-1);
    }

    return result;
}

std::string FormatLocalDateTime(std::time_t value, const char* format)
{
    if (!format) return std::string();

    const std::tm* local = std::localtime(&value);
    if (!local) return std::string();

    char buffer[64] = {0};
    if (std::strftime(buffer, sizeof(buffer), format, local) == 0) {
        return std::string();
    }
    return buffer;
}

std::time_t AddLocalCalendarDays(std::time_t value, int days)
{
    const std::tm* current = std::localtime(&value);
    if (!current) return static_cast<std::time_t>(-1);

    std::tm adjusted = *current;
    adjusted.tm_mday += days;
    adjusted.tm_isdst = -1;
    return std::mktime(&adjusted);
}

bool EvaluateLoanDateTime(const std::string& loan_date,
                          int duration_days,
                          std::time_t now,
                          LoanEvaluation* evaluation)
{
    if (!evaluation || duration_days <= 0) return false;

    const std::time_t loan_time = ParseLoanDateTime(loan_date);
    if (loan_time == static_cast<std::time_t>(-1)) return false;

    const std::time_t expiry_time = AddLocalCalendarDays(loan_time, duration_days);
    if (expiry_time == static_cast<std::time_t>(-1)) return false;

    LoanEvaluation result;
    result.loan_time = loan_time;
    result.expiry_time = expiry_time;
    if (!CalculateLocalDaysRemaining(
            expiry_time, now, &result.days_remaining)) {
        return false;
    }
    result.future_dated = loan_time > now;
    result.expired = expiry_time < now;
    result.visible_in_operational_list = expiry_time >= now;
    *evaluation = result;
    return true;
}

bool BuildQuotaWindowBounds(std::time_t now,
                            int monthly_window_days,
                            QuotaWindowBounds* bounds)
{
    if (!bounds || monthly_window_days <= 0) return false;

    const std::string date = FormatLocalDateTime(now, "%Y-%m-%d");
    if (date.empty()) return false;

    const std::time_t monthly_start =
        AddLocalCalendarDays(now, -monthly_window_days);
    if (monthly_start == static_cast<std::time_t>(-1)) return false;

    const std::string monthly =
        FormatLocalDateTime(monthly_start, "%Y-%m-%d %H:%M");
    if (monthly.empty()) return false;

    QuotaWindowBounds result;
    result.daily_since = date + " 00:00";
    result.monthly_since = monthly;
    *bounds = result;
    return true;
}

bool CalculateQuotaAvailability(int daily_limit,
                                int daily_count,
                                int monthly_limit,
                                int monthly_count,
                                QuotaAvailability* availability)
{
    if (!availability || daily_limit <= 0 || monthly_limit <= 0 ||
        daily_count < 0 || monthly_count < 0) {
        return false;
    }

    QuotaAvailability result;
    result.daily_remaining = daily_count >= daily_limit
        ? 0 : daily_limit - daily_count;
    result.monthly_remaining = monthly_count >= monthly_limit
        ? 0 : monthly_limit - monthly_count;
    *availability = result;
    return true;
}

} // namespace biblioled
