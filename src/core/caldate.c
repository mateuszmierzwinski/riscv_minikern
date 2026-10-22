#include "core/caldate.h"

#define DAYS_PER_YEAR 365u
#define EPOCH_YEAR    1970u

static const uint8_t MONTH_DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

bool caldate_is_leap(uint32_t year) {
    /* Every fourth year, except centuries, except every fourth century —
     * all three clauses, because the one that is usually left out is the
     * one that makes 2100 wrong. */
    return (year % 4u == 0 && year % 100u != 0) || year % 400u == 0;
}

static uint32_t days_in_month(uint32_t year, uint32_t month) {
    uint32_t days = MONTH_DAYS[month - 1u];
    return month == 2u && caldate_is_leap(year) ? days + 1u : days;
}

/* Whole days from the epoch to the first of this year. */
static uint64_t days_before_year(uint32_t year) {
    uint64_t days = 0;
    for (uint32_t y = EPOCH_YEAR; y < year; y++) {
        days += DAYS_PER_YEAR + (caldate_is_leap(y) ? 1u : 0u);
    }
    return days;
}

static bool fields_are_a_date(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute,
                              uint32_t second) {
    if (year < EPOCH_YEAR || month < 1u || month > 12u || day < 1u) {
        return false;
    }
    if (day > days_in_month(year, month)) {
        return false;
    }
    /* 60 seconds is allowed nowhere here: a leap second is not a thing this
     * clock reports, and accepting one would move every later instant. */
    return hour < 24u && minute < 60u && second < 60u;
}

uint64_t caldate_to_unix(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute,
                         uint32_t second) {
    uint64_t days;
    if (!fields_are_a_date(year, month, day, hour, minute, second)) {
        return 0;
    }
    days = days_before_year(year);
    for (uint32_t m = 1u; m < month; m++) {
        days += days_in_month(year, m);
    }
    days += day - 1u;
    return ((days * 24ull + hour) * 60ull + minute) * 60ull + second;
}
