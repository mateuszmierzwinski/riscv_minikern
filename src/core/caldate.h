/* caldate — a calendar date to a unix time, and nothing else.
 *
 * The real-time clock holds a date, and everything above it wants seconds
 * since 1970. The conversion is pure arithmetic over leap years and month
 * lengths, which makes it the testable half of an RTC driver — the half
 * that is wrong in February 2100 if it is wrong at all.
 */
#ifndef MINIKERN_CORE_CALDATE_H
#define MINIKERN_CORE_CALDATE_H

#include <stdbool.h>
#include <stdint.h>

/* Seconds from 1970-01-01 00:00:00 UTC to this date and time, or 0 when
 * the fields are not a real date (which is also how an unset clock reads). */
uint64_t caldate_to_unix(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute,
                         uint32_t second);

/* True for a leap year in the proleptic Gregorian calendar. */
bool caldate_is_leap(uint32_t year);

#endif
