<script setup>
import { ref, computed, watch, onMounted } from "vue";
import { useTheme } from "vuetify";
import { useAppStore, useSettingsStore } from "../stores";
import { themeOptions, THEME_STORAGE_KEY, DEFAULT_THEME } from "../plugins/vuetify";
import AppLogo from "./AppLogo.vue";

const appStore = useAppStore();
const settingsStore = useSettingsStore();
const sleepDialog = ref(false);
const rotating = ref(false);
const skipping = ref(false);
const skipSteps = ref(1);

const theme = useTheme();
const currentTheme = ref(DEFAULT_THEME);

function applyTheme(name) {
  theme.global.name.value = name;
  currentTheme.value = name;
  localStorage.setItem(THEME_STORAGE_KEY, name);
}

onMounted(() => {
  const saved = localStorage.getItem(THEME_STORAGE_KEY);
  if (saved && themeOptions.some((t) => t.value === saved)) {
    applyTheme(saved);
  }
});

const deviceTitle = computed(
  () => settingsStore.deviceSettings.deviceName?.trim() || "ESP32 PhotoFrame"
);

watch(
  deviceTitle,
  (title) => {
    document.title = title;
  },
  { immediate: true }
);

async function handleSleep() {
  await appStore.enterSleep();
  sleepDialog.value = false;
}

async function handleRotate() {
  rotating.value = true;
  await appStore.rotateNow();
  rotating.value = false;
}

async function handleSkip(steps) {
  if (!steps) return;
  skipping.value = true;
  await appStore.skipQueue(steps);
  skipping.value = false;
}

function getBatteryColor(level) {
  if (level < 20) return "error";
  if (level < 50) return "warning";
  return "success";
}
</script>

<template>
  <v-app-bar color="primary" density="comfortable">
    <template #prepend>
      <AppLogo :size="40" class="ml-2" />
    </template>

    <v-app-bar-title class="ml-4">{{ deviceTitle }}</v-app-bar-title>

    <template #append>
      <!-- Theme Menu -->
      <v-menu>
        <template #activator="{ props }">
          <v-btn icon class="mr-2" title="Theme" v-bind="props">
            <v-icon>mdi-palette</v-icon>
          </v-btn>
        </template>
        <v-list density="compact">
          <v-list-item
            v-for="opt in themeOptions"
            :key="opt.value"
            :active="opt.value === currentTheme"
            @click="applyTheme(opt.value)"
          >
            <v-list-item-title>{{ opt.title }}</v-list-item-title>
          </v-list-item>
        </v-list>
      </v-menu>

      <!-- Skip in Queue Menu -->
      <v-menu :close-on-content-click="false">
        <template #activator="{ props }">
          <v-btn icon class="mr-2" title="Skip in queue" v-bind="props">
            <v-icon>mdi-debug-step-over</v-icon>
          </v-btn>
        </template>
        <v-card class="pa-3" min-width="240">
          <div class="text-subtitle-2">Skip in queue</div>
          <div class="text-caption text-medium-emphasis mb-2" style="max-width: 220px">
            One-time jump and refresh now — not a permanent setting (it won't
            skip on every rotation).
          </div>
          <div class="d-flex align-center" style="gap: 8px">
            <v-btn
              icon="mdi-chevron-double-left"
              size="small"
              variant="tonal"
              :loading="skipping"
              title="Jump back"
              @click="handleSkip(-Math.abs(skipSteps || 1))"
            ></v-btn>
            <v-text-field
              v-model.number="skipSteps"
              type="number"
              min="1"
              density="compact"
              variant="outlined"
              hide-details
              style="max-width: 72px"
            ></v-text-field>
            <v-btn
              icon="mdi-chevron-double-right"
              size="small"
              variant="tonal"
              :loading="skipping"
              title="Jump forward"
              @click="handleSkip(Math.abs(skipSteps || 1))"
            ></v-btn>
          </div>
        </v-card>
      </v-menu>

      <!-- Rotate Now Button -->
      <v-btn
        icon
        class="mr-2"
        :loading="rotating"
        title="Rotate to next image"
        @click="handleRotate"
      >
        <v-icon>mdi-rotate-right</v-icon>
      </v-btn>

      <!-- Sleep Button -->
      <v-btn icon class="mr-2" title="Enter Deep Sleep" @click="sleepDialog = true">
        <v-icon>mdi-sleep</v-icon>
      </v-btn>

      <!-- Battery Status -->
      <v-chip
        v-if="appStore.battery.connected"
        variant="flat"
        :color="getBatteryColor(appStore.battery.level)"
        class="text-white"
      >
        <v-icon :icon="appStore.battery.charging ? 'mdi-battery-charging' : 'mdi-battery'" start />
        {{ appStore.battery.level }}%
        <span v-if="appStore.battery.charging" class="ml-1">Charging</span>
      </v-chip>
      <v-chip v-else variant="flat" color="grey-darken-1" class="mr-2 text-white">
        <v-icon icon="mdi-power-plug-off" start />
        No Battery
      </v-chip>
    </template>
  </v-app-bar>

  <!-- Sleep Confirmation Dialog -->
  <v-dialog v-model="sleepDialog" max-width="400">
    <v-card>
      <v-card-title>Enter Deep Sleep?</v-card-title>
      <v-card-text>
        The device will enter deep sleep mode and stop responding until woken up.
      </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="text" @click="sleepDialog = false"> Cancel </v-btn>
        <v-btn color="primary" @click="handleSleep"> Sleep </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>
</template>
