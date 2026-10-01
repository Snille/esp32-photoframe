// Timezone choices for a frame. The frame stores a POSIX TZ string and applies
// it with setenv("TZ")/tzset(), so a zone with DST rules switches between
// summer and winter time on its own. The old UI only offered a plain UTC offset
// ("UTC-2"), which never follows DST and could not even display a DST string —
// it showed 0 and wrote "UTC0" back on save.

// Zones that observe DST (or have a non-whole-hour offset), by region.
const NAMED_ZONES = [
  {
    title: "Stockholm, Berlin, Paris (CET/CEST)",
    value: "CET-1CEST,M3.5.0,M10.5.0/3",
  },
  { title: "London, Dublin (GMT/BST)", value: "GMT0BST,M3.5.0/1,M10.5.0" },
  { title: "Lisbon (WET/WEST)", value: "WET0WEST,M3.5.0/1,M10.5.0" },
  {
    title: "Helsinki, Athens (EET/EEST)",
    value: "EET-2EEST,M3.5.0/3,M10.5.0/4",
  },
  { title: "Moscow (MSK)", value: "MSK-3" },
  { title: "New York (EST/EDT)", value: "EST5EDT,M3.2.0,M11.1.0" },
  { title: "Chicago (CST/CDT)", value: "CST6CDT,M3.2.0,M11.1.0" },
  { title: "Denver (MST/MDT)", value: "MST7MDT,M3.2.0,M11.1.0" },
  { title: "Phoenix (MST, no DST)", value: "MST7" },
  { title: "Los Angeles (PST/PDT)", value: "PST8PDT,M3.2.0,M11.1.0" },
  { title: "Anchorage (AKST/AKDT)", value: "AKST9AKDT,M3.2.0,M11.1.0" },
  { title: "Honolulu (HST)", value: "HST10" },
  { title: "India (IST, UTC+5:30)", value: "IST-5:30" },
  { title: "China, Taiwan (CST, UTC+8)", value: "CST-8" },
  { title: "Japan (JST, UTC+9)", value: "JST-9" },
  {
    title: "Sydney, Melbourne (AEST/AEDT)",
    value: "AEST-10AEDT,M10.1.0,M4.1.0/3",
  },
  { title: "Auckland (NZST/NZDT)", value: "NZST-12NZDT,M9.5.0,M4.1.0/3" },
];

// Plain whole-hour offsets, no DST. POSIX inverts the sign: UTC+2 is "UTC-2".
const FIXED_OFFSETS = Array.from({ length: 27 }, (_, i) => {
  const east = i - 12; // -12 .. +14
  const label = east === 0 ? "UTC" : `UTC${east > 0 ? "+" : ""}${east}`;
  const value = east === 0 ? "UTC0" : `UTC${east > 0 ? "-" : "+"}${Math.abs(east)}`;
  return { title: `${label} (fixed, no daylight saving)`, value };
});

export const TIMEZONE_OPTIONS = [...NAMED_ZONES, ...FIXED_OFFSETS];

// The options to show for a frame whose current value is `current`. A value the
// list doesn't know (set on the frame itself, or by an older UI) is added as its
// own entry so it is shown as-is and saved back unchanged.
export function timezoneOptionsFor(current) {
  if (!current || TIMEZONE_OPTIONS.some((o) => o.value === current)) {
    return TIMEZONE_OPTIONS;
  }
  return [{ title: `${current} (custom)`, value: current }, ...TIMEZONE_OPTIONS];
}
