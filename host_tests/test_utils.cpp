/**
 * Google Test-based unit tests for calculate_next_wakeup_interval
 */

#include <gtest/gtest.h>
#include <time.h>

#include <cstring>

#include "../main/testable_utils.h"

// Test fixture
class CalculateNextWakeupIntervalTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        // Reset config before each test
        config.enabled = false;
        config.start_minutes = 1380;  // 23:00
        config.end_minutes = 420;     // 07:00
        SetMockTime(0, 0, 0);
    }

    void SetMockTime(int hour, int minute, int second)
    {
        memset(&timeinfo, 0, sizeof(timeinfo));
        timeinfo.tm_hour = hour;
        timeinfo.tm_min = minute;
        timeinfo.tm_sec = second;
        timeinfo.tm_year = 126;  // 2026
        timeinfo.tm_mon = 0;     // January
        timeinfo.tm_mday = 20;
    }

    struct tm timeinfo;
    sleep_schedule_config_t config;
};

// Test Case 1: No sleep schedule - simple clock alignment
TEST_F(CalculateNextWakeupIntervalTest, NoSleepSchedule1HourInterval)
{
    config.enabled = false;
    SetMockTime(10, 30, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, 0, &config);

    EXPECT_EQ(1800, result) << "Should wake in 30 minutes (at 11:00)";
}

// Test Case 2: No sleep schedule - 30 minute interval
TEST_F(CalculateNextWakeupIntervalTest, NoSleepSchedule30MinInterval)
{
    config.enabled = false;
    SetMockTime(10, 15, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 1800, true, 0, &config);

    EXPECT_EQ(900, result) << "Should wake in 15 minutes (at 10:30)";
}

// Test Case 3: Sleep schedule enabled, next wake-up is outside schedule
TEST_F(CalculateNextWakeupIntervalTest, SleepScheduleWakeOutside)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(18, 0, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, 0, &config);

    EXPECT_EQ(3600, result) << "Should wake in 1 hour (at 19:00)";
}

// Test Case 4: Sleep schedule enabled, next wake-up would be inside schedule
TEST_F(CalculateNextWakeupIntervalTest, SleepScheduleWakeInside)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(22, 30, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, 0, &config);

    EXPECT_EQ(30600, result)
        << "Should skip to 07:00 next day (8.5 hours) - sleep_end is exclusive";
}

// Test Case 5: Currently in sleep schedule
TEST_F(CalculateNextWakeupIntervalTest, CurrentlyInSleepSchedule)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(2, 0, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, 0, &config);

    EXPECT_EQ(18000, result) << "Should wake at 07:00 (5 hours) - sleep_end is exclusive";
}

// Test Case 6: Sleep schedule ends at aligned time
TEST_F(CalculateNextWakeupIntervalTest, SleepScheduleEndsAtAlignedTime)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(6, 0, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, 0, &config);

    EXPECT_EQ(3600, result) << "Should wake at 07:00 (1 hour)";
}

// Test Case 7: Sleep schedule with 2-hour interval
TEST_F(CalculateNextWakeupIntervalTest, SleepSchedule2HourInterval)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 435;     // 07:15
    SetMockTime(22, 0, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 7200, true, 0, &config);

    EXPECT_EQ(36000, result)
        << "Should skip to 08:00 next day (10 hours) - first aligned time >= sleep_end";
}

// Test Case 8: Same-day schedule (not overnight)
TEST_F(CalculateNextWakeupIntervalTest, SameDaySchedule)
{
    config.enabled = true;
    config.start_minutes = 720;  // 12:00
    config.end_minutes = 840;    // 14:00
    SetMockTime(11, 30, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, 0, &config);

    EXPECT_EQ(9000, result) << "Should skip to 14:00 (2.5 hours) - sleep_end is exclusive";
}

// Test Case 9: Edge case - exactly at midnight
TEST_F(CalculateNextWakeupIntervalTest, ExactlyAtMidnight)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(0, 0, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, 0, &config);

    EXPECT_EQ(25200, result) << "Should wake at 07:00 (7 hours) - sleep_end is exclusive";
}

// Test Case 10: 15-minute interval
TEST_F(CalculateNextWakeupIntervalTest, FifteenMinuteInterval)
{
    config.enabled = false;
    SetMockTime(10, 7, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 900, true, 0, &config);

    EXPECT_EQ(480, result) << "Should wake at 10:15 (8 minutes)";
}

// Test Case 11: Time drift - woke up 40 seconds early, should skip to next interval
TEST_F(CalculateNextWakeupIntervalTest, TimeDriftWokeUpEarly)
{
    config.enabled = false;
    SetMockTime(16, 59, 20);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, 0, &config);

    EXPECT_EQ(3640, result) << "Should skip to 18:00 since 40s < 60s threshold";
}

// New tests for Non-Aligned mode
TEST_F(CalculateNextWakeupIntervalTest, NonAlignedWakeOutsideSchedule)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(18, 5, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, false, 0, &config);

    EXPECT_EQ(3600, result) << "Should wake exactly in 1 hour (at 19:05)";
}

TEST_F(CalculateNextWakeupIntervalTest, NonAlignedWakeInsideSchedule)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(22, 30, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, false, 0, &config);

    // 22:30 + 1 hour = 23:30 (inside schedule)
    // Should wake at 07:00 next day (8.5 hours = 30600 seconds)
    EXPECT_EQ(30600, result) << "Should skip to 07:00 next day";
}

TEST_F(CalculateNextWakeupIntervalTest, NonAlignedCurrentlyInSchedule)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(2, 0, 0);         // Currently 02:00 (inside schedule)

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, false, 0, &config);

    EXPECT_EQ(18000, result) << "Should wake at 07:00 (5 hours)";
}

TEST_F(CalculateNextWakeupIntervalTest, NonAlignedOvernightCrossMidnight)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(22, 30, 0);       // 22:30, 30 mins before sleep schedule

    // Interval is 1 hour, so next wake at 23:30 (inside schedule)
    int result = calculate_next_wakeup_interval(&timeinfo, 3600, false, 0, &config);

    EXPECT_EQ(30600, result) << "Should wake at 07:00 next day (8.5 hours)";
}

TEST_F(CalculateNextWakeupIntervalTest, NonAlignedSameDaySchedule)
{
    config.enabled = true;
    config.start_minutes = 720;  // 12:00
    config.end_minutes = 840;    // 14:00
    SetMockTime(11, 30, 0);      // 11:30, 30 mins before sleep schedule

    // Interval is 1 hour, so next wake at 12:30 (inside schedule)
    int result = calculate_next_wakeup_interval(&timeinfo, 3600, false, 0, &config);

    EXPECT_EQ(9000, result) << "Should wake at 14:00 (2.5 hours)";
}

TEST_F(CalculateNextWakeupIntervalTest, SameDayScheduleWraparound)
{
    config.enabled = true;
    config.start_minutes = 0;  // 00:00
    config.end_minutes = 480;  // 08:00
    SetMockTime(23, 40, 51);   // Current time 23:40:51

    // Rotation interval 1 hour aligned
    // Next aligned time would be 00:00 (tomorrow)
    // 00:00 falls in schedule [00:00, 08:00)
    // So should skip to 08:00 tomorrow.

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, 0, &config);

    // 23:40:51 to 08:00:00 next day
    // 23:40:51 -> 24:00:00 = 19m 9s = 1149s
    // 00:00:00 -> 08:00:00 = 8h = 28800s
    // Total = 29949s
    EXPECT_EQ(29949, result) << "Should wake at 08:00 tomorrow (wrapper around)";
}

// --- Rotation offset (staggering frames that share an interval) ---

TEST_F(CalculateNextWakeupIntervalTest, OffsetShiftsAlignedGrid)
{
    config.enabled = false;
    SetMockTime(10, 20, 0);

    // 15-minute interval offset by 5 minutes -> grid is :05/:20/:35/:50.
    // Note 10:20 is itself a slot, so the next one is 10:35.
    int result = calculate_next_wakeup_interval(&timeinfo, 900, true, 300, &config);

    EXPECT_EQ(900, result) << "Should wake at 10:35 on the offset grid";
}

TEST_F(CalculateNextWakeupIntervalTest, OffsetSeparatesTwoFramesOnSameInterval)
{
    config.enabled = false;
    SetMockTime(10, 1, 0);

    int frame_a = calculate_next_wakeup_interval(&timeinfo, 1800, true, 0, &config);
    int frame_b = calculate_next_wakeup_interval(&timeinfo, 1800, true, 600, &config);

    EXPECT_EQ(1740, frame_a) << "Unshifted frame wakes at 10:30";
    EXPECT_EQ(540, frame_b) << "Shifted frame wakes at 10:10";
    EXPECT_NE(frame_a, frame_b) << "The whole point: they must not collide";
}

TEST_F(CalculateNextWakeupIntervalTest, ZeroOffsetMatchesPlainAlignment)
{
    config.enabled = false;
    SetMockTime(14, 7, 30);

    // An offset of a whole interval is the same grid as no offset at all.
    int without = calculate_next_wakeup_interval(&timeinfo, 900, true, 0, &config);
    int wrapped = calculate_next_wakeup_interval(&timeinfo, 900, true, 900, &config);

    EXPECT_EQ(450, without) << "Should wake at 14:15";
    EXPECT_EQ(without, wrapped) << "offset == interval normalizes to 0";
}

TEST_F(CalculateNextWakeupIntervalTest, NegativeOffsetIsNormalized)
{
    config.enabled = false;
    SetMockTime(10, 20, 0);

    // -600 on a 900s grid is the same as +300.
    int negative = calculate_next_wakeup_interval(&timeinfo, 900, true, -600, &config);
    int positive = calculate_next_wakeup_interval(&timeinfo, 900, true, 300, &config);

    EXPECT_EQ(positive, negative) << "Negative offsets wrap into [0, interval)";
}

TEST_F(CalculateNextWakeupIntervalTest, OffsetBeforeFirstSlotOfDay)
{
    config.enabled = false;
    SetMockTime(0, 1, 0);

    // 00:01 with a 5-minute offset on an hourly grid -> first slot is 00:05.
    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, 300, &config);

    EXPECT_EQ(240, result) << "Should wake at 00:05, not skip to 01:05";
}

TEST_F(CalculateNextWakeupIntervalTest, OffsetTooSoonSkipsToNextSlot)
{
    config.enabled = false;
    SetMockTime(10, 4, 30);

    // Next offset slot (10:05) is only 30s out -> skip a slot to avoid an
    // immediate re-wake from clock drift, same rule as the unshifted grid.
    int result = calculate_next_wakeup_interval(&timeinfo, 900, true, 300, &config);

    EXPECT_EQ(930, result) << "Should skip to 10:20";
}

TEST_F(CalculateNextWakeupIntervalTest, OffsetRespectedAtEndOfSleepWindow)
{
    config.enabled = true;
    config.start_minutes = 1320;  // 22:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(23, 30, 0);

    // First offset slot at or after 07:00 is 07:05.
    int result = calculate_next_wakeup_interval(&timeinfo, 900, true, 300, &config);

    // 23:30 -> midnight = 1800s, midnight -> 07:05 = 25500s
    EXPECT_EQ(27300, result) << "Should wake at 07:05, on the offset grid";
}

// --- Unset clock (no SNTP sync yet) ---

TEST_F(CalculateNextWakeupIntervalTest, UnsetClockWithOffsetDoesNotWakeEarly)
{
    config.enabled = false;
    SetMockTime(0, 0, 0);
    timeinfo.tm_year = 70;  // 1970: the clock has never been set

    // Seconds-of-day reads 0, which is before the day's first offset slot.
    // Aligning would target that slot and sleep only `offset` seconds — a frame
    // on a 5-minute interval waking every 2 minutes, flattening its battery.
    int result = calculate_next_wakeup_interval(&timeinfo, 300, true, 120, &config);

    EXPECT_EQ(300, result) << "Should fall back to a plain interval, not the 120s slot";
}

TEST_F(CalculateNextWakeupIntervalTest, UnsetClockWithoutOffsetAlsoUsesPlainInterval)
{
    config.enabled = false;
    SetMockTime(0, 0, 0);
    timeinfo.tm_year = 70;

    int result = calculate_next_wakeup_interval(&timeinfo, 900, true, 0, &config);

    EXPECT_EQ(900, result) << "No grid to align to before the clock is set";
}

TEST_F(CalculateNextWakeupIntervalTest, ClockSetRestoresAlignment)
{
    config.enabled = false;
    SetMockTime(10, 20, 0);  // SetUp already puts tm_year at 2026

    // Same call as above but with a real clock: the grid applies again, so the
    // frame snaps back on its first wake after a successful time sync.
    int result = calculate_next_wakeup_interval(&timeinfo, 900, true, 300, &config);

    EXPECT_EQ(900, result) << "Should wake at 10:35 on the offset grid";
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
