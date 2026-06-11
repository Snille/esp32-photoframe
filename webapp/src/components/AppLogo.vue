<template>
  <svg
    :width="size"
    :height="size"
    viewBox="0 0 512 512"
    role="img"
    aria-label="PhotoFrame"
  >
    <defs>
      <linearGradient :id="shellId" x1="0" y1="0" x2="1" y2="1">
        <stop offset="0%" :stop-color="c.shellStart" />
        <stop offset="100%" :stop-color="c.shellEnd" />
      </linearGradient>
    </defs>

    <!-- Shell / frame border: follows the theme primary (darker shade so it
         stays visible on the primary-coloured app bar) -->
    <rect x="0" y="0" width="512" height="512" rx="96" :fill="`url(#${shellId})`" />

    <!-- Frame body + photo: kept warm for contrast against the themed shell -->
    <rect x="96" y="112" width="320" height="288" rx="20" :fill="frameBody" />
    <rect x="120" y="136" width="272" height="240" rx="8" :fill="picBg" />

    <!-- Sun + horizon: accent details that also follow the theme primary -->
    <circle cx="328" cy="200" r="28" :fill="c.sun" fill-opacity="0.95" />
    <path
      d="M 120 320 Q 200 260 280 300 Q 340 320 392 306 L 392 376 L 120 376 Z"
      :fill="c.hill1"
      fill-opacity="0.7"
    />
    <path
      d="M 120 350 Q 180 320 240 336 Q 320 360 392 344 L 392 376 L 120 376 Z"
      :fill="c.hill2"
      fill-opacity="0.85"
    />

    <rect x="236" y="400" width="40" height="12" rx="4" :fill="frameBody" fill-opacity="0.9" />
  </svg>
</template>

<script setup>
import { computed } from "vue";
import { useTheme } from "vuetify";

defineProps({ size: { type: [Number, String], default: 40 } });

// Unique gradient id so multiple logos on a page don't clash.
const shellId = `pf-shell-${Math.random().toString(36).slice(2, 8)}`;

// Warm constants — the photo inside the frame stays warm for contrast.
const frameBody = "#faf5eb";
const picBg = "#f4c896";

const clamp = (n) => Math.max(0, Math.min(255, n));
const hexToRgb = (hex) => {
  const h = hex.replace("#", "");
  const full = h.length === 3 ? h.replace(/(.)/g, "$1$1") : h;
  return [
    parseInt(full.slice(0, 2), 16),
    parseInt(full.slice(2, 4), 16),
    parseInt(full.slice(4, 6), 16),
  ];
};
const rgbToHex = (r, g, b) =>
  "#" +
  [r, g, b].map((x) => clamp(Math.round(x)).toString(16).padStart(2, "0")).join("");
// amt in [-1, 1]: positive lightens toward white, negative darkens toward black.
const shade = (hex, amt) => {
  const [r, g, b] = hexToRgb(hex);
  if (amt >= 0) {
    return rgbToHex(r + (255 - r) * amt, g + (255 - g) * amt, b + (255 - b) * amt);
  }
  const k = 1 + amt;
  return rgbToHex(r * k, g * k, b * k);
};

const theme = useTheme();
const c = computed(() => {
  const primary = theme.current.value.colors.primary || "#ce9160";
  return {
    shellStart: shade(primary, -0.45),
    shellEnd: shade(primary, -0.1),
    sun: shade(primary, 0.35),
    hill1: primary,
    hill2: shade(primary, -0.2),
  };
});
</script>
