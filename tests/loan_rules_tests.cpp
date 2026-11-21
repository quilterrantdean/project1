#include "biblioled/loan_rules.h"

#include <cstdlib>
#include <iostream>
#include <ctime>
#include <string>

#ifdef _WIN32
#include <stdlib.h>
#else
#include <cstdlib>
#endif

namespace {

int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

std::time_t Parse(const char* value)
{
    return biblioled::ParseLoanDateTime(value);
}

void TestDateParsing()
{
    Check(Parse("2024-02-29 12:34") != static_cast<std::time_t>(-1),
          "a valid leap-day date should parse");
    Check(Parse("2023-02-29 12:34") == static_cast<std::time_t>(-1),
          "an invalid leap-day date should be rejected");
    Check(Parse("2024-13-01 12:34") == static_cast<std::time_t>(-1),
          "an out-of-range month should be rejected");
    Check(Parse("2024-01-01 24:00") == static_cast<std::time_t>(-1),
          "an out-of-range hour should be rejected");
    Check(Parse("2024-01-01 12:60") == static_cast<std::time_t>(-1),
          "an out-of-range minute should be rejected");
    Check(Parse("2024-01-01 12:00 trailing") == static_cast<std::time_t>(-1),
          "trailing text should be rejected");
    Check(Parse("1899-12-31 23:59") == static_cast<std::time_t>(-1),
          "years before 1900 should be rejected");
    Check(Parse("2024-2-29 12:34") == static_cast<std::time_t>(-1),
          "single-digit month should be rejected");
    Check(Parse("2024-02-9 12:34") == static_cast<std::time_t>(-1),
          "single-digit day should be rejected");
    Check(Parse("2024-02-29 2:34") == static_cast<std::time_t>(-1),
          "single-digit hour should be rejected");
    Check(Parse("2024-02-29 12:4") == static_cast<std::time_t>(-1),
          "single-digit minute should be rejected");
    Check(Parse("+2024-02-29 12:34") == static_cast<std::time_t>(-1),
          "signed year should be rejected");
    Check(Parse("2024-+2-29 12:34") == static_cast<std::time_t>(-1),
          "signed month should be rejected");
    Check(Parse(" 2024-02-29 12:34") == static_cast<std::time_t>(-1),
          "leading whitespace should be rejected");
    Check(Parse("2024-02-29 12:34 ") == static_cast<std::time_t>(-1),
          "trailing whitespace should be rejected");
    Check(Parse("2024-02-29T12:34") == static_cast<std::time_t>(-1),
          "non-canonical date/time separator should be rejected");
    Check(Parse("2024/02/29 12:34") == static_cast<std::time_t>(-1),
          "non-canonical date separators should be rejected");
}

void TestCalendarArithmetic()
{
    const std::time_t january_end = Parse("2024-01-31 15:30");
    Check(january_end != static_cast<std::time_t>(-1),
          "January test date should parse");
    const std::time_t february_start =
        biblioled::AddLocalCalendarDays(january_end, 1);
    Check(february_start != static_cast<std::time_t>(-1),
          "adding one calendar day should succeed");
    Check(biblioled::FormatLocalDateTime(
              february_start, "%Y-%m-%d %H:%M") == "2024-02-01 15:30",
          "calendar-day arithmetic should cross month boundaries and preserve local time");

    const std::time_t leap_day = Parse("2024-02-28 09:15");
    const std::time_t next_day =
        biblioled::AddLocalCalendarDays(leap_day, 1);
    Check(biblioled::FormatLocalDateTime(
              next_day, "%Y-%m-%d %H:%M") == "2024-02-29 09:15",
          "calendar-day arithmetic should include leap day");
}

void TestLoanEvaluation()
{
    const std::time_t now = Parse("2024-06-01 12:00");
    Check(now != static_cast<std::time_t>(-1), "reference time should parse");

    biblioled::LoanEvaluation future;
    Check(biblioled::EvaluateLoanDateTime(
              "2024-06-05 12:00", 21, now, &future),
          "future-dated loan should be evaluated");
    Check(future.future_dated && !future.expired &&
          future.visible_in_operational_list,
          "future-dated loans should remain visible in the operational list");

    biblioled::LoanEvaluation active;
    Check(biblioled::EvaluateLoanDateTime(
              "2024-05-20 12:00", 21, now, &active),
          "active loan should be evaluated");
    Check(!active.future_dated && !active.expired &&
          active.visible_in_operational_list,
          "unexpired past loans should remain visible");
    Check(active.days_remaining == 9,
          "remaining days should retain the existing whole-day calculation");

    biblioled::LoanEvaluation expired;
    Check(biblioled::EvaluateLoanDateTime(
              "2024-05-01 12:00", 21, now, &expired),
          "expired loan should be evaluated");
    Check(expired.expired && !expired.visible_in_operational_list,
          "expired loans should be omitted from the operational list");
    Check(expired.days_remaining == -10,
          "expired-loan day difference should retain the existing calculation");

    biblioled::LoanEvaluation boundary;
    Check(biblioled::EvaluateLoanDateTime(
              "2024-05-11 12:00", 21, now, &boundary),
          "loan expiring exactly now should be evaluated");
    Check(boundary.expiry_time == now &&
          boundary.visible_in_operational_list &&
          !boundary.expired,
          "expiry equal to now should remain visible");

    Check(!biblioled::EvaluateLoanDateTime(
              "2024-05-20 12:00", 0, now, &active),
          "zero loan duration should be rejected");
    Check(!biblioled::EvaluateLoanDateTime(
              "invalid", 21, now, &active),
          "invalid loan date should not be evaluated");
}

void TestCalendarDayDifference()
{
    const char* previous = std::getenv("TZ");
    const bool had_previous = previous != NULL;
    const std::string previous_value = had_previous ? previous : "";

#ifdef _WIN32
    Check(_putenv("TZ=GMT0BST,M3.5.0/1,M10.5.0/2") == 0,
          "test timezone should be configured");
    _tzset();
#else
    Check(setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1) == 0,
          "test timezone should be configured");
    tzset();
#endif

    const std::time_t before_spring_change = Parse("2024-03-10 12:00");
    biblioled::LoanEvaluation daylight_saving;
    Check(biblioled::EvaluateLoanDateTime(
              "2024-03-10 12:00", 21, before_spring_change, &daylight_saving),
          "loan spanning the spring clock change should be evaluated");
    Check(daylight_saving.days_remaining == 21,
          "days remaining should count local calendar days across the spring clock change");

    const std::time_t just_after_expiry = Parse("2024-03-31 12:01");
    biblioled::LoanEvaluation expired;
    Check(biblioled::EvaluateLoanDateTime(
              "2024-03-10 12:00", 21, just_after_expiry, &expired),
          "recently expired loan should be evaluated");
    Check(expired.expired && expired.days_remaining == -1,
          "a partially elapsed negative day should follow calendar-day floor semantics");

#ifdef _WIN32
    const std::string restore = had_previous ? "TZ=" + previous_value : "TZ=";
    _putenv(restore.c_str());
    _tzset();
#else
    if (had_previous) setenv("TZ", previous_value.c_str(), 1);
    else unsetenv("TZ");
    tzset();
#endif
}

void TestQuotaWindows()
{
    const std::time_t now = Parse("2024-06-20 15:45");
    biblioled::QuotaWindowBounds bounds;
    Check(biblioled::BuildQuotaWindowBounds(now, 30, &bounds),
          "quota windows should be calculated");
    Check(bounds.daily_since == "2024-06-20 00:00",
          "daily quota should begin at local midnight");
    Check(bounds.monthly_since == "2024-05-21 15:45",
          "monthly quota should use a rolling calendar-day window");
    Check(!biblioled::BuildQuotaWindowBounds(now, 0, &bounds),
          "zero monthly window should be rejected");
    Check(!biblioled::BuildQuotaWindowBounds(now, 30, NULL),
          "null quota-window output should be rejected");
}

void TestQuotaAvailability()
{
    biblioled::QuotaAvailability availability;
    Check(biblioled::CalculateQuotaAvailability(5, 3, 20, 7, &availability),
          "remaining quotas should be calculated");
    Check(availability.daily_remaining == 2 &&
          availability.monthly_remaining == 13,
          "quota availability should subtract existing counts");

    Check(biblioled::CalculateQuotaAvailability(5, 5, 20, 20, &availability),
          "quotas at their limits should be calculated");
    Check(availability.daily_remaining == 0 &&
          availability.monthly_remaining == 0,
          "quotas at their limits should be zero");

    Check(biblioled::CalculateQuotaAvailability(5, 8, 20, 25, &availability),
          "counts above limits should be calculated");
    Check(availability.daily_remaining == 0 &&
          availability.monthly_remaining == 0,
          "counts above limits should clamp at zero");

    Check(!biblioled::CalculateQuotaAvailability(0, 0, 20, 0, &availability),
          "zero quota limits should be rejected");
    Check(!biblioled::CalculateQuotaAvailability(5, -1, 20, 0, &availability),
          "negative loan counts should be rejected");
    Check(!biblioled::CalculateQuotaAvailability(5, 0, 20, 0, NULL),
          "null quota availability output should be rejected");
}

} // namespace

int main()
{
    TestDateParsing();
    TestCalendarArithmetic();
    TestLoanEvaluation();
    TestCalendarDayDifference();
    TestQuotaWindows();
    TestQuotaAvailability();

    if (failures != 0) {
        std::cerr << failures << " loan-rules test(s) failed." << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "All loan-rules tests passed." << std::endl;
    return EXIT_SUCCESS;
}
