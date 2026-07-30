#include "testable_utils.h"

// True when the clock has plausibly been set (SNTP synced, or restored from the
// RTC). An unsynced ESP32 sits in 1970, where "seconds since midnight" is
// meaningless — and so is any wake grid derived from it.
static bool clock_is_set(const struct tm *timeinfo)
{
    return (timeinfo->tm_year + 1900) >= 2020;
}

// Normalize a configured offset into [0, rotate_interval). A negative or
// oversized value is a config mistake, not a reason to compute a bogus wake.
static int normalize_offset(int rotate_offset, int rotate_interval)
{
    if (rotate_interval <= 0) {
        return 0;
    }
    int offset = rotate_offset % rotate_interval;
    if (offset < 0) {
        offset += rotate_interval;
    }
    return offset;
}

int calculate_next_wakeup_interval(const struct tm *timeinfo, int rotate_interval, bool aligned,
                                   int rotate_offset, const sleep_schedule_config_t *sleep_schedule)
{
    int current_seconds_of_day =
        timeinfo->tm_hour * 3600 + timeinfo->tm_min * 60 + timeinfo->tm_sec;
    int seconds_until_next;
    int offset = normalize_offset(rotate_offset, rotate_interval);

    // Aligning to a clock that has not been set produces a wake time derived
    // from 1970, which is arbitrary — and with an offset it is actively harmful:
    // seconds-of-day reads ~0, which is *before* the day's first offset slot, so
    // the frame targets that slot and sleeps only `offset` seconds. Observed as a
    // frame on a 5-minute interval waking every 2 minutes with a 2-minute offset,
    // draining its battery. Without an offset the same path happened to land a
    // whole interval out, which is why it went unnoticed. Fall back to a plain
    // interval wait; the first wake after the clock is set snaps back to the grid.
    bool align_now = aligned && clock_is_set(timeinfo);

    if (align_now) {
        // The alignment grid is shifted by `offset` seconds, so several frames
        // sharing an interval can keep clock-aligned wakes without all hitting
        // the server (and the WiFi) in the same second. offset == 0 reproduces
        // the plain top-of-the-hour grid exactly.
        int relative_seconds = current_seconds_of_day - offset;
        int next_aligned_seconds;
        if (relative_seconds < 0) {
            // Before today's first slot (e.g. 00:02 with a 5-minute offset).
            next_aligned_seconds = offset;
        } else {
            next_aligned_seconds =
                ((relative_seconds / rotate_interval) + 1) * rotate_interval + offset;
        }
        seconds_until_next = next_aligned_seconds - current_seconds_of_day;

        // If next wakeup is too soon (less than 60s), skip to the following interval.
        // This prevents immediate re-wakeup due to time drift.
        if (seconds_until_next < 60) {
            next_aligned_seconds += rotate_interval;
            seconds_until_next = next_aligned_seconds - current_seconds_of_day;
        }
    } else {
        seconds_until_next = rotate_interval;
    }

    // Check if sleep schedule is enabled
    if (sleep_schedule == NULL || !sleep_schedule->enabled) {
        return seconds_until_next;
    }

    // Calculate the wake-up time in seconds since midnight
    int wake_seconds_of_day = current_seconds_of_day + seconds_until_next;

    // Normalize wake_seconds_of_day to handle day overflow
    while (wake_seconds_of_day >= 86400) {
        wake_seconds_of_day -= 86400;
    }

    int sleep_start_seconds = sleep_schedule->start_minutes * 60;
    int sleep_end_seconds = sleep_schedule->end_minutes * 60;

    // Check if wake-up time falls within or at the start of sleep schedule
    bool wake_in_schedule = false;
    if (sleep_start_seconds > sleep_end_seconds) {
        // Schedule crosses midnight (e.g., 23:00 - 07:00)
        // Wake is in schedule if >= start OR < end
        wake_in_schedule =
            (wake_seconds_of_day >= sleep_start_seconds || wake_seconds_of_day < sleep_end_seconds);
    } else {
        // Schedule within same day (e.g., 12:00 - 14:00)
        // Wake is in schedule if >= start AND < end
        wake_in_schedule =
            (wake_seconds_of_day >= sleep_start_seconds && wake_seconds_of_day < sleep_end_seconds);
    }

    if (!wake_in_schedule) {
        // Wake-up is outside sleep schedule, use normal interval
        return seconds_until_next;
    }

    // Wake-up would be in sleep schedule, calculate next wake-up time at or after schedule ends.
    long long next_wake_seconds_of_day;
    if (align_now) {
        // Find the first aligned time >= sleep_end (sleep_end is exclusive),
        // on the same offset-shifted grid used above.
        long long relative_end = (long long) sleep_end_seconds - offset;
        if (relative_end <= 0) {
            next_wake_seconds_of_day = offset;
        } else {
            next_wake_seconds_of_day =
                (relative_end + rotate_interval - 1) / rotate_interval * rotate_interval + offset;
        }
    } else {
        // For non-aligned rotation, just wake up exactly when the sleep schedule ends
        next_wake_seconds_of_day = sleep_end_seconds;
    }

    // Calculate seconds from current time to next wake-up
    int seconds_until_wake;
    if (sleep_start_seconds > sleep_end_seconds) {
        // Overnight schedule (e.g., 23:00 - 07:00)
        if (current_seconds_of_day >= sleep_start_seconds ||
            current_seconds_of_day < sleep_end_seconds) {
            // Currently in the schedule
            if (current_seconds_of_day >= sleep_start_seconds) {
                // Before midnight - wake after schedule ends next day
                seconds_until_wake =
                    (86400 - current_seconds_of_day) + (int) next_wake_seconds_of_day;
            } else {
                // After midnight - wake at next aligned time today
                seconds_until_wake = (int) next_wake_seconds_of_day - current_seconds_of_day;
            }
        } else {
            // Currently between schedule end and start
            // But wake time is in schedule, so skip to next day
            seconds_until_wake = (86400 - current_seconds_of_day) + (int) next_wake_seconds_of_day;
        }
    } else {
        // Same-day schedule
        seconds_until_wake = (int) next_wake_seconds_of_day - current_seconds_of_day;
        if (seconds_until_wake < 0) {
            seconds_until_wake += 86400;  // Wrap to next day
        }
    }

    return seconds_until_wake;
}
