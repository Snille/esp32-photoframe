<script setup>
import { ref, computed, onMounted, onUnmounted } from "vue";
import { useSettingsStore, useAppStore } from "../stores";
import PaletteCalibration from "./PaletteCalibration.vue";
import ProcessingControls from "./ProcessingControls.vue";

const settingsStore = useSettingsStore();
const appStore = useAppStore();

// Selectable actions for the wake-button gestures (must match firmware ids).
const buttonActionOptions = [
  { title: "Do nothing", value: "none" },
  { title: "Next image", value: "next_image" },
  { title: "Go to deep sleep", value: "sleep" },
  { title: "Toggle deep sleep on/off", value: "toggle_deep_sleep" },
  { title: "Show info screen", value: "info_screen" },
];

// Boards with persistent storage support in-browser upload (and thus the
// client-side AI keys). SRAM-only boards hide those fields.
const hasUploadSupport = computed(
  () => appStore.systemInfo.sdcard_inserted || appStore.systemInfo.has_flash_storage
);

// SRAM-only boards (no PSRAM, e.g. the 4MB FireBeetle) can't complete a TLS
// handshake alongside the permanently-held 120KB framebuffer, so https:// image
// URLs fail with out-of-memory errors and never rotate. system-info exposes
// https_supported=false on those boards. Treat a missing flag as supported so
// older firmware doesn't get a false warning.
const httpsUnsupported = computed(() => appStore.systemInfo.https_supported === false);

const imageUrlIsHttps = computed(() =>
  /^\s*https:\/\//i.test(settingsStore.deviceSettings.imageUrl || "")
);

// Block saving only when this board can't do HTTPS *and* an https:// URL is set
// in URL rotation mode — otherwise the user would save a config that can never
// fetch an image.
const httpsUrlBlocked = computed(
  () =>
    httpsUnsupported.value &&
    imageUrlIsHttps.value &&
    settingsStore.deviceSettings.rotationMode === "url"
);

// Device time state
const deviceTime = ref("");
const syncingTime = ref(false);
let deviceTimestamp = null; // Unix timestamp from device
let localTimeOffset = 0; // Offset between device time and local time
let tickInterval = null;

function updateDisplayTime() {
  if (deviceTimestamp === null) return;
  // Calculate current device time based on elapsed local time
  const elapsed = Math.floor((Date.now() - localTimeOffset) / 1000);
  const currentTimestamp = deviceTimestamp + elapsed;

  // Apply timezone offset for display
  // We shift the timestamp by the offset so that toISOString() (which is UTC)
  // displays the correct local time numbers.
  const offsetHours = settingsStore.deviceSettings.timezoneOffset || 0;
  const adjustedTimestamp = currentTimestamp + offsetHours * 3600;

  const date = new Date(adjustedTimestamp * 1000);
  // Format as YYYY-MM-DD HH:MM:SS
  deviceTime.value = date.toISOString().slice(0, 19).replace("T", " ");
}

async function parseTimezone(timezoneStr) {
  if (!timezoneStr) return;

  // Posix format: UTC[+/-]H[:MM] (e.g., UTC-8 or UTC+5:30)
  // Note: POSIX sign is inverted relative to ISO8601
  let offset = 0;
  const match = timezoneStr.match(/UTC([+-]?)(\d+)(?::(\d+))?/);
  if (match) {
    const sign = match[1] === "-" ? 1 : -1; // POSIX Inverted
    const hours = parseInt(match[2]) || 0;
    const minutes = parseInt(match[3]) || 0;
    offset = sign * (hours + minutes / 60);

    // Update store if different, to keep UI in sync
    if (settingsStore.deviceSettings.timezoneOffset !== offset) {
      settingsStore.deviceSettings.timezoneOffset = offset;
    }
  }
}

async function fetchDeviceTime() {
  try {
    const response = await fetch("/api/time");
    if (response.ok) {
      const data = await response.json();
      deviceTimestamp = data.timestamp;
      localTimeOffset = Date.now();
      await parseTimezone(data.timezone);
      updateDisplayTime();
    }
  } catch (error) {
    console.error("Failed to fetch device time:", error);
  }
}

async function syncTime() {
  syncingTime.value = true;
  try {
    const response = await fetch("/api/time/sync", { method: "POST" });
    if (response.ok) {
      const data = await response.json();
      if (data.status === "success") {
        deviceTimestamp = data.timestamp;
        localTimeOffset = Date.now();
        await parseTimezone(data.timezone);
        updateDisplayTime();
      }
    }
  } catch (error) {
    console.error("Failed to sync time:", error);
  } finally {
    syncingTime.value = false;
  }
}

// --- Battery voltage calibration (one-point multimeter) ---
// Applies to any board that reads the pack through an ADC divider; the frame
// scales every reading so its reported voltage matches a value measured with a
// multimeter on the resting cell. Data comes straight from /api/battery so this
// widget is independent of the batched config save.
const batteryCal = ref({ supportsCal: false, calScale: 1, voltageMv: null, level: null });
const measuredVoltage = ref("");
const calibrating = ref(false);
const calMessage = ref("");
const calError = ref(false);

async function fetchBatteryStatus() {
  try {
    const res = await fetch("/api/battery");
    if (!res.ok) return;
    const b = await res.json();
    batteryCal.value = {
      supportsCal: !!b.supports_cal,
      calScale: b.cal_scale ?? 1,
      voltageMv: b.battery_voltage,
      level: b.battery_level,
    };
  } catch (error) {
    console.error("Failed to fetch battery status:", error);
  }
}

function applyCalResult(r) {
  batteryCal.value.calScale = r.cal_scale;
  batteryCal.value.voltageMv = r.voltage_mv;
  batteryCal.value.level = r.battery_level;
}

async function calibrateBattery() {
  const volts = parseFloat(measuredVoltage.value);
  if (!(volts > 0)) {
    calError.value = true;
    calMessage.value = "Enter the measured cell voltage in volts (e.g. 4.19)";
    setTimeout(() => (calError.value = false), 5000);
    return;
  }
  calibrating.value = true;
  calMessage.value = "";
  try {
    const res = await fetch("/api/battery/calibrate", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ measured_mv: Math.round(volts * 1000) }),
    });
    if (res.ok) {
      const r = await res.json();
      applyCalResult(r);
      measuredVoltage.value = "";
      calError.value = false;
      calMessage.value = `Calibrated — now reads ${(r.voltage_mv / 1000).toFixed(2)} V (${
        r.battery_level
      }%), scale ${Number(r.cal_scale).toFixed(3)}`;
      setTimeout(() => (calMessage.value = ""), 6000);
    } else {
      calError.value = true;
      calMessage.value = (await res.text()) || "Calibration failed";
      setTimeout(() => (calError.value = false), 8000);
    }
  } catch (error) {
    console.error("Battery calibration failed:", error);
    calError.value = true;
    calMessage.value = "Calibration request failed";
    setTimeout(() => (calError.value = false), 8000);
  } finally {
    calibrating.value = false;
  }
}

async function resetBatteryCalibration() {
  calibrating.value = true;
  calMessage.value = "";
  try {
    const res = await fetch("/api/battery/calibrate", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ measured_mv: 0 }),
    });
    if (res.ok) {
      applyCalResult(await res.json());
      calError.value = false;
      calMessage.value = "Reset to factory calibration";
      setTimeout(() => (calMessage.value = ""), 5000);
    }
  } catch (error) {
    console.error("Battery calibration reset failed:", error);
  } finally {
    calibrating.value = false;
  }
}

onMounted(() => {
  fetchDeviceTime();
  fetchBatteryStatus();
  // Tick every second to update display
  tickInterval = setInterval(updateDisplayTime, 1000);
});

onUnmounted(() => {
  if (tickInterval) {
    clearInterval(tickInterval);
  }
});

const tab = computed({
  get: () => settingsStore.activeSettingsTab,
  set: (val) => (settingsStore.activeSettingsTab = val),
});

const orientationOptions = computed(() => {
  const width = appStore.systemInfo.width || 800;
  const height = appStore.systemInfo.height || 480;
  const maxDim = Math.max(width, height);
  const minDim = Math.min(width, height);

  return [
    { title: `Landscape (${maxDim}×${minDim})`, value: "landscape" },
    { title: `Portrait (${minDim}×${maxDim})`, value: "portrait" },
  ];
});

const rotationOptions = [
  { title: "0°", value: 0 },
  { title: "90°", value: 90 },
  { title: "180°", value: 180 },
  { title: "270°", value: 270 },
];

const rotationModeOptions = computed(() => {
  const options = [{ title: "URL - Fetch image from URL", value: "url" }];
  if (appStore.systemInfo.sdcard_inserted || appStore.systemInfo.has_flash_storage) {
    options.unshift({ title: "Storage - Rotate through images", value: "storage" });
  }
  return options;
});

const sdRotationModeOptions = [
  { title: "Random - Shuffle images", value: "random" },
  { title: "Sequential - In sequence", value: "sequential" },
];

const saving = ref(false);
const saveSuccess = ref(false);

function onPresetChange(preset) {
  if (preset !== "custom") {
    settingsStore.applyPreset(preset);
  }
}

function onParamsUpdate(newParams) {
  Object.assign(settingsStore.params, newParams);
}

const saveMessage = ref("");
const saveError = ref(false);

const showFactoryResetDialog = ref(false);
const resetting = ref(false);
const showFlashModeDialog = ref(false);
const enteringFlashMode = ref(false);
const showImportDialog = ref(false);
const importData = ref(null);
const importFileName = ref("");

async function exportConfig() {
  try {
    const [configRes, processingRes, paletteRes] = await Promise.all([
      fetch("/api/config"),
      fetch("/api/settings/processing"),
      fetch("/api/settings/palette"),
    ]);

    const exported = {};

    if (configRes.ok) {
      const config = await configRes.json();
      // Remove sensitive fields
      delete config.wifi_password;
      exported.config = config;
    }
    if (processingRes.ok) exported.processing = await processingRes.json();
    if (paletteRes.ok) exported.palette = await paletteRes.json();

    const blob = new Blob([JSON.stringify(exported, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    const deviceName = settingsStore.deviceSettings.deviceName || "photoframe";
    a.download = `${deviceName.toLowerCase().replace(/\s+/g, "-")}-config.json`;
    a.click();
    URL.revokeObjectURL(url);
  } catch (error) {
    console.error("Failed to export config:", error);
  }
}

function onImportFileSelected(event) {
  const file = event.target.files?.[0];
  if (!file) return;

  importFileName.value = file.name;
  const reader = new FileReader();
  reader.onload = (e) => {
    try {
      importData.value = JSON.parse(e.target.result);
      showImportDialog.value = true;
    } catch {
      saveError.value = true;
      saveMessage.value = "Invalid JSON file";
      setTimeout(() => (saveError.value = false), 5000);
    }
  };
  reader.readAsText(file);
  // Reset input so the same file can be selected again
  event.target.value = "";
}

async function performImport() {
  if (!importData.value) return;

  showImportDialog.value = false;
  saving.value = true;

  try {
    const promises = [];

    if (importData.value.config) {
      promises.push(
        fetch("/api/config", {
          method: "PATCH",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(importData.value.config),
        })
      );
    }
    if (importData.value.processing) {
      promises.push(
        fetch("/api/settings/processing", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(importData.value.processing),
        })
      );
    }
    if (importData.value.palette) {
      promises.push(
        fetch("/api/settings/palette", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(importData.value.palette),
        })
      );
    }

    await Promise.all(promises);

    // Reload all settings from device
    await Promise.all([
      settingsStore.loadDeviceSettings(),
      settingsStore.loadSettings(),
      settingsStore.loadPalette(),
    ]);

    saveSuccess.value = true;
    saveError.value = false;
    saveMessage.value = "Config imported successfully!";
    setTimeout(() => (saveSuccess.value = false), 3000);
  } catch (error) {
    console.error("Failed to import config:", error);
    saveError.value = true;
    saveMessage.value = "Failed to import config";
    setTimeout(() => (saveError.value = false), 5000);
  } finally {
    saving.value = false;
    importData.value = null;
  }
}

async function saveSettings() {
  saving.value = true;

  // Save both device settings and processing settings
  const [deviceResult, processingSuccess] = await Promise.all([
    settingsStore.saveDeviceSettings(),
    settingsStore.saveSettings(),
  ]);

  saving.value = false;

  if (deviceResult.success && processingSuccess) {
    saveSuccess.value = true;
    saveError.value = false;
    saveMessage.value = deviceResult.message || "Settings saved!";
    setTimeout(() => (saveSuccess.value = false), 3000);

    // Refresh device time in case timezone changed
    await fetchDeviceTime();
  } else {
    // Show error message
    saveError.value = true;
    saveSuccess.value = false;
    saveMessage.value = deviceResult.message || "Failed to save settings";
    setTimeout(() => (saveError.value = false), 5000);
  }
}

async function performFactoryReset() {
  resetting.value = true;
  const result = await settingsStore.factoryReset();
  resetting.value = false;
  showFactoryResetDialog.value = false;

  if (result.success) {
    saveSuccess.value = true;
    saveError.value = false;
    saveMessage.value = result.message;
    setTimeout(() => (saveSuccess.value = false), 3000);
  } else {
    saveError.value = true;
    saveSuccess.value = false;
    saveMessage.value = result.message;
    setTimeout(() => (saveError.value = false), 5000);
  }
}

async function enterFlashMode() {
  enteringFlashMode.value = true;
  const result = await settingsStore.enterDownloadMode();
  enteringFlashMode.value = false;
  showFlashModeDialog.value = false;

  saveSuccess.value = result.success;
  saveError.value = !result.success;
  saveMessage.value = result.message;
  setTimeout(() => {
    saveSuccess.value = false;
    saveError.value = false;
  }, 6000);
}
</script>

<template>
  <div>
    <v-card style="overflow: visible">
      <v-card-title class="d-flex align-center">
        <v-icon icon="mdi-cog" class="mr-2" />
        Settings
      </v-card-title>

      <v-tabs v-model="tab" color="primary" show-arrows density="compact">
        <v-tab value="general"> General </v-tab>
        <v-tab value="autoRotate"> Auto Rotate </v-tab>
        <v-tab value="power"> Power </v-tab>
        <v-tab value="homeAssistant"> Home Assistant </v-tab>
        <v-tab value="processing"> Processing </v-tab>
        <v-tab value="ai"> AI Generation </v-tab>
        <v-tab value="calibration"> Palette </v-tab>
        <v-tab value="maintenance"> Maintenance </v-tab>
      </v-tabs>

      <v-card-text>
        <v-tabs-window v-model="tab">
          <!-- General Tab -->
          <v-tabs-window-item value="general">
            <v-row class="mt-2">
              <v-col cols="12" md="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.deviceName"
                  label="Device Name"
                  variant="outlined"
                  hint="Used for mDNS hostname (e.g., 'Living Room Frame' → living-room-frame.local)"
                  persistent-hint
                />
              </v-col>
            </v-row>

            <v-row>
              <v-col cols="12" md="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.wifiSsid"
                  label="WiFi SSID"
                  variant="outlined"
                  hint="Network name to connect to"
                  persistent-hint
                />
              </v-col>
              <v-col cols="12" md="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.wifiPassword"
                  label="WiFi Password"
                  type="password"
                  variant="outlined"
                  hint="Leave empty to keep current password"
                  persistent-hint
                  placeholder="••••••••"
                />
              </v-col>
            </v-row>

            <v-row>
              <v-col cols="12" md="6">
                <v-select
                  v-model="settingsStore.deviceSettings.displayOrientation"
                  :items="orientationOptions"
                  item-title="title"
                  item-value="value"
                  label="Display Orientation"
                  variant="outlined"
                />
              </v-col>
              <v-col cols="12" md="6">
                <v-select
                  v-model="settingsStore.deviceSettings.displayRotationDeg"
                  :items="rotationOptions"
                  item-title="title"
                  item-value="value"
                  label="Display Rotation (deg)"
                  variant="outlined"
                />
              </v-col>
            </v-row>

            <v-row>
              <v-col cols="12" md="6">
                <v-text-field
                  :model-value="deviceTime || 'Loading...'"
                  label="Device Time"
                  variant="outlined"
                  readonly
                  hint="Click sync to update from NTP server"
                  persistent-hint
                >
                  <template #append-inner>
                    <v-btn
                      icon
                      variant="text"
                      size="small"
                      :loading="syncingTime"
                      @click="syncTime"
                    >
                      <v-icon>mdi-sync</v-icon>
                      <v-tooltip activator="parent" location="top">Sync NTP</v-tooltip>
                    </v-btn>
                  </template>
                </v-text-field>
              </v-col>
              <v-col cols="12" md="6">
                <v-text-field
                  v-model.number="settingsStore.deviceSettings.timezoneOffset"
                  label="Timezone (UTC offset)"
                  type="number"
                  :min="-12"
                  :max="14"
                  :step="0.5"
                  variant="outlined"
                  hint="e.g., -8 for PST, +1 for CET, +8 for CST"
                  persistent-hint
                />
              </v-col>
            </v-row>
            <v-row>
              <v-col cols="12" md="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.ntpServer"
                  label="NTP Server"
                  variant="outlined"
                  hint="e.g., pool.ntp.org, cn.pool.ntp.org"
                  persistent-hint
                />
              </v-col>
            </v-row>
          </v-tabs-window-item>

          <!-- Auto Rotate Tab -->
          <v-tabs-window-item value="autoRotate">
            <v-switch
              v-model="settingsStore.deviceSettings.autoRotate"
              label="Enable Auto-Rotate"
              color="primary"
              class="mb-2"
              hide-details
            />

            <div class="ml-10">
              <v-row class="mb-0">
                <v-col cols="6" md="3">
                  <v-text-field
                    v-model.number="settingsStore.deviceSettings.rotateHours"
                    label="Hours"
                    type="number"
                    :min="0"
                    :max="23"
                    variant="outlined"
                    :disabled="!settingsStore.deviceSettings.autoRotate"
                    hide-details
                  />
                </v-col>
                <v-col cols="6" md="3">
                  <v-text-field
                    v-model.number="settingsStore.deviceSettings.rotateMinutes"
                    label="Minutes"
                    type="number"
                    :min="0"
                    :max="59"
                    variant="outlined"
                    :disabled="!settingsStore.deviceSettings.autoRotate"
                    hide-details
                  />
                </v-col>
              </v-row>

              <v-checkbox
                v-model="settingsStore.deviceSettings.autoRotateAligned"
                label="Align rotation to clock boundaries"
                hide-details
                class="mb-0 ml-2"
                :disabled="!settingsStore.deviceSettings.autoRotate"
              />
              <v-alert
                v-if="settingsStore.deviceSettings.autoRotateAligned"
                type="info"
                variant="tonal"
                density="compact"
              >
                Rotation aligns to clock boundaries. 1 hour rotates at 1:00, 2:00, etc.
              </v-alert>

              <v-select
                v-model="settingsStore.deviceSettings.rotationMode"
                :items="rotationModeOptions"
                item-title="title"
                item-value="value"
                label="Rotation Mode"
                variant="outlined"
                class="mt-8 mb-4"
                :disabled="!settingsStore.deviceSettings.autoRotate"
              />

              <v-expand-transition>
                <v-card
                  v-if="
                    settingsStore.deviceSettings.autoRotate &&
                    settingsStore.deviceSettings.rotationMode === 'storage'
                  "
                  variant="tonal"
                  class="mb-4"
                >
                  <v-card-text>
                    <v-select
                      v-model="settingsStore.deviceSettings.sdRotationMode"
                      :items="sdRotationModeOptions"
                      item-title="title"
                      item-value="value"
                      label="Storage Rotation Logic"
                      variant="outlined"
                      hide-details
                    />
                  </v-card-text>
                </v-card>
              </v-expand-transition>

              <v-expand-transition>
                <v-card
                  v-if="
                    settingsStore.deviceSettings.autoRotate &&
                    settingsStore.deviceSettings.rotationMode === 'url'
                  "
                  variant="tonal"
                  class="mb-4"
                >
                  <v-card-text>
                    <v-text-field
                      v-model="settingsStore.deviceSettings.imageUrl"
                      label="Image URL"
                      variant="outlined"
                      class="mb-2"
                      :error="httpsUrlBlocked"
                      :error-messages="
                        httpsUrlBlocked
                          ? 'HTTPS is not supported on this board — use an http:// URL.'
                          : []
                      "
                      :hint="
                        httpsUnsupported
                          ? 'This board has no PSRAM; only http:// image URLs work for rotation.'
                          : undefined
                      "
                      :persistent-hint="httpsUnsupported"
                    />

                    <v-alert
                      v-if="httpsUrlBlocked"
                      type="warning"
                      variant="tonal"
                      density="compact"
                      class="mb-4"
                    >
                      HTTPS image URLs are not supported on this board (no PSRAM — the TLS handshake
                      runs out of memory). Use an
                      <strong>http://</strong> URL, e.g. your photoframe server on the local
                      network.
                    </v-alert>

                    <div
                      v-if="settingsStore.deviceSettings.caCertSet && !httpsUnsupported"
                      class="mb-4 d-flex flex-column ga-1"
                    >
                      <v-chip
                        color="success"
                        size="small"
                        variant="tonal"
                        style="align-self: flex-start"
                      >
                        <v-icon start>mdi-check-circle</v-icon>
                        Certificate Pinned
                      </v-chip>
                      <div class="text-caption text-medium-emphasis">
                        The TLS certificate for this HTTPS URL is pinned. It will re-pin
                        automatically when you change the URL.
                      </div>
                    </div>

                    <v-alert
                      v-if="settingsStore.deviceSettings.lastFetchError"
                      type="error"
                      variant="tonal"
                      density="compact"
                      class="mb-4"
                    >
                      Last fetch error: {{ settingsStore.deviceSettings.lastFetchError }}
                    </v-alert>

                    <v-checkbox
                      v-if="
                        appStore.systemInfo.sdcard_inserted || appStore.systemInfo.has_flash_storage
                      "
                      v-model="settingsStore.deviceSettings.saveDownloadedImages"
                      label="Save downloaded images to Downloads album"
                      color="primary"
                      class="mb-8"
                      hide-details
                    />

                    <v-text-field
                      v-model="settingsStore.deviceSettings.accessToken"
                      label="Access Token (Optional)"
                      variant="outlined"
                      hint="Sets Authorization: Bearer header"
                      persistent-hint
                      class="mt-4"
                    />

                    <v-row class="mt-4">
                      <v-col cols="12" md="6">
                        <v-text-field
                          v-model="settingsStore.deviceSettings.httpHeaderKey"
                          label="Custom Header Name"
                          variant="outlined"
                          placeholder="e.g., X-API-Key"
                        />
                      </v-col>
                      <v-col cols="12" md="6">
                        <v-text-field
                          v-model="settingsStore.deviceSettings.httpHeaderValue"
                          label="Custom Header Value"
                          variant="outlined"
                        />
                      </v-col>
                    </v-row>
                  </v-card-text>
                </v-card>
              </v-expand-transition>
            </div>

            <v-divider class="my-4" />

            <v-switch
              v-model="settingsStore.deviceSettings.sleepScheduleEnabled"
              label="Enable Sleep Schedule"
              color="primary"
              class="mb-2"
              hide-details
            />
            <div class="ml-10">
              <v-row>
                <v-col cols="6" md="3">
                  <v-text-field
                    v-model="settingsStore.deviceSettings.sleepScheduleStart"
                    label="From"
                    type="time"
                    variant="outlined"
                    :disabled="!settingsStore.deviceSettings.sleepScheduleEnabled"
                  />
                </v-col>
                <v-col cols="6" md="3">
                  <v-text-field
                    v-model="settingsStore.deviceSettings.sleepScheduleEnd"
                    label="To"
                    type="time"
                    variant="outlined"
                    :disabled="!settingsStore.deviceSettings.sleepScheduleEnabled"
                  />
                </v-col>
              </v-row>

              <v-alert type="info" variant="tonal" density="compact">
                Images won't rotate during this period. Useful for night hours.
              </v-alert>
            </div>
          </v-tabs-window-item>

          <!-- Power Tab -->
          <v-tabs-window-item value="power">
            <v-switch
              v-model="settingsStore.deviceSettings.deepSleepEnabled"
              label="Enable Deep Sleep"
              color="primary"
              class="mb-4"
            />

            <v-expand-transition>
              <v-alert
                v-if="!settingsStore.deviceSettings.deepSleepEnabled"
                type="warning"
                variant="tonal"
              >
                <strong>Power Consumption Notice</strong><br />
                Disabling deep sleep keeps the HTTP server accessible but significantly increases
                power consumption. Only disable if permanently powered via USB.
              </v-alert>
            </v-expand-transition>

            <template v-if="settingsStore.deviceSettings.batteryAdcGpioOptions.length">
              <v-divider class="my-4" />

              <div class="text-subtitle-1 mb-1">Battery Voltage (external divider)</div>
              <div class="text-caption text-medium-emphasis mb-3">
                This board has no built-in battery sensor. If you've wired an external 1MΩ+1MΩ
                divider from VBAT to one of the pins below, select it here to enable battery
                reporting.
              </div>
              <v-select
                v-model="settingsStore.deviceSettings.batteryAdcGpio"
                :items="[
                  { title: 'Not connected', value: null },
                  ...settingsStore.deviceSettings.batteryAdcGpioOptions.map((o) => ({
                    title: o.label,
                    value: o.gpio,
                  })),
                ]"
                label="Battery ADC pin"
                variant="outlined"
                density="compact"
              />
            </template>

            <template v-if="batteryCal.supportsCal">
              <v-divider class="my-4" />

              <div class="text-subtitle-1 mb-1">Battery Voltage Calibration</div>
              <div class="text-caption text-medium-emphasis mb-3">
                Fine-tune this frame's reported battery voltage to its true value. Measure the
                resting cell with a multimeter, enter it below, and the frame scales every reading
                to match — fixing a battery that never reaches 100% or reports a skewed level.
              </div>
              <div class="text-body-2 mb-2">
                Currently reports:
                <strong>{{
                  batteryCal.voltageMv != null
                    ? (batteryCal.voltageMv / 1000).toFixed(2) + " V"
                    : "—"
                }}</strong>
                <span v-if="batteryCal.level != null"> ({{ batteryCal.level }}%)</span>
                · scale {{ Number(batteryCal.calScale).toFixed(3) }}
              </div>
              <div class="d-flex align-center flex-wrap" style="gap: 8px">
                <v-text-field
                  v-model="measuredVoltage"
                  label="Measured voltage (V)"
                  placeholder="4.19"
                  type="number"
                  step="0.01"
                  variant="outlined"
                  density="compact"
                  hide-details
                  style="max-width: 200px"
                />
                <v-btn
                  color="primary"
                  :loading="calibrating"
                  :disabled="calibrating"
                  @click="calibrateBattery"
                >
                  Calibrate
                </v-btn>
                <v-btn variant="text" :disabled="calibrating" @click="resetBatteryCalibration">
                  Reset
                </v-btn>
              </div>
              <v-alert
                v-if="calMessage"
                :type="calError ? 'error' : 'success'"
                variant="tonal"
                density="compact"
                class="mt-2"
              >
                {{ calMessage }}
              </v-alert>
            </template>

            <v-divider class="my-4" />

            <div class="text-subtitle-1 mb-1">Button</div>
            <div class="text-caption text-medium-emphasis mb-3">
              Choose what the wake button does while the frame is awake. A press always wakes the
              frame from deep sleep first.
            </div>
            <v-select
              v-model="settingsStore.deviceSettings.buttonActionShort"
              :items="buttonActionOptions"
              label="Short press (&lt;2s)"
              variant="outlined"
              density="compact"
              class="mb-2"
            />
            <v-select
              v-model="settingsStore.deviceSettings.buttonActionLong"
              :items="buttonActionOptions"
              label="Long press (2–5s)"
              variant="outlined"
              density="compact"
              class="mb-2"
            />
            <v-select
              v-model="settingsStore.deviceSettings.buttonActionHold"
              :items="buttonActionOptions"
              label="Hold (≥5s)"
              variant="outlined"
              density="compact"
            />
          </v-tabs-window-item>

          <!-- Home Assistant Tab -->
          <v-tabs-window-item class="mt-2" value="homeAssistant">
            <v-text-field
              v-model="settingsStore.deviceSettings.haUrl"
              label="Home Assistant URL"
              variant="outlined"
              placeholder="http://homeassistant.local:8123"
              hint="Configure for dynamic image serving and battery level reporting"
              persistent-hint
            />
          </v-tabs-window-item>

          <!-- Processing Tab -->
          <v-tabs-window-item value="processing">
            <div class="pa-4">
              <ProcessingControls
                :params="settingsStore.params"
                :preset="settingsStore.preset"
                @update:params="onParamsUpdate"
                @update:preset="settingsStore.preset = $event"
                @preset-change="onPresetChange"
              />
            </div>
          </v-tabs-window-item>

          <!-- AI Generation Tab -->
          <v-tabs-window-item value="ai">
            <v-alert type="info" variant="tonal" density="compact" class="mt-2 mb-4">
              Prompt is used for server-side AI generation (e.g. ComfyUI) when the image source is
              set to AI Generation.<template v-if="hasUploadSupport">
                API keys below are only for client-side generation when uploading images.</template
              >
            </v-alert>

            <v-textarea
              v-model="settingsStore.deviceSettings.aiPrompt"
              label="AI Prompt"
              variant="outlined"
              rows="3"
              auto-grow
              placeholder="A serene Swedish summer landscape at golden hour..."
              hint="Sent to the server for AI Generation. Overrides the prompt stored on the server for this device."
              persistent-hint
              class="mb-4"
            />

            <!-- Client-side AI keys are only used by the in-browser upload path,
                 which needs persistent storage; hidden on SRAM-only boards. -->
            <template v-if="hasUploadSupport">
              <v-divider class="mb-4" />

              <v-text-field
                v-model="settingsStore.deviceSettings.aiCredentials.openaiApiKey"
                label="OpenAI API Key"
                variant="outlined"
                type="password"
                hint="sk-..."
                persistent-hint
                class="mb-2"
              />
              <div class="text-caption text-grey ml-2 mb-4">
                Get your API key at
                <a
                  href="https://platform.openai.com/api-keys"
                  target="_blank"
                  class="text-primary text-decoration-none"
                  >platform.openai.com</a
                >
              </div>

              <v-text-field
                v-model="settingsStore.deviceSettings.aiCredentials.googleApiKey"
                label="Google Gemini API Key"
                variant="outlined"
                type="password"
                class="mb-2"
              />
              <div class="text-caption text-grey ml-2 mb-4">
                Get your API key at
                <a
                  href="https://aistudio.google.com/app/apikey"
                  target="_blank"
                  class="text-primary text-decoration-none"
                  >aistudio.google.com</a
                >
              </div>
            </template>
          </v-tabs-window-item>

          <!-- Calibration Tab -->
          <v-tabs-window-item value="calibration">
            <PaletteCalibration />
          </v-tabs-window-item>

          <!-- Maintenance Tab -->
          <v-tabs-window-item value="maintenance">
            <div class="text-subtitle-1 mt-2 mb-4">Config Backup</div>
            <v-row>
              <v-col cols="12">
                <v-btn variant="outlined" class="mr-2" @click="exportConfig">
                  <v-icon start>mdi-download</v-icon>
                  Export Config
                </v-btn>
                <v-btn variant="outlined" @click="$refs.importInput.click()">
                  <v-icon start>mdi-upload</v-icon>
                  Import Config
                </v-btn>
                <input
                  ref="importInput"
                  type="file"
                  accept=".json"
                  style="display: none"
                  @change="onImportFileSelected"
                />
              </v-col>
            </v-row>

            <template v-if="appStore.systemInfo.download_mode_supported">
              <v-divider class="my-6" />

              <div class="text-subtitle-1 mb-4">Firmware Flashing</div>
              <v-row>
                <v-col cols="12">
                  <v-btn variant="outlined" @click="showFlashModeDialog = true">
                    <v-icon start>mdi-usb-flash-drive</v-icon>
                    Enter Flash Mode
                  </v-btn>
                  <div class="text-caption text-grey mt-2">
                    Reboots the device into USB flash/download mode so you can flash new firmware
                    over USB without pressing BOOT/RST. The device goes offline until you flash it
                    or power-cycle it.
                  </div>
                </v-col>
              </v-row>
            </template>

            <v-divider class="my-6" />

            <div class="text-subtitle-1 mb-4">Factory Reset</div>
            <v-row>
              <v-col cols="12">
                <v-btn color="error" variant="outlined" @click="showFactoryResetDialog = true">
                  <v-icon start>mdi-restore-alert</v-icon>
                  Factory Reset Device
                </v-btn>
              </v-col>
            </v-row>
          </v-tabs-window-item>
        </v-tabs-window>
      </v-card-text>

      <v-card-actions class="px-4 pb-4">
        <v-spacer />
        <v-fade-transition>
          <v-chip v-if="saveSuccess" color="success" variant="tonal">
            <v-icon icon="mdi-check" start />
            {{ saveMessage || "Settings saved!" }}
          </v-chip>
          <v-chip v-else-if="saveError" color="error" variant="tonal">
            <v-icon icon="mdi-alert-circle" start />
            {{ saveMessage || "Failed to save settings" }}
          </v-chip>
        </v-fade-transition>
        <v-btn color="primary" :loading="saving" :disabled="httpsUrlBlocked" @click="saveSettings">
          <v-icon icon="mdi-content-save" start />
          Save Settings
        </v-btn>
      </v-card-actions>
    </v-card>

    <!-- Factory Reset Confirmation Dialog -->
    <v-dialog v-model="showFactoryResetDialog" max-width="500">
      <v-card>
        <v-card-title class="text-h5 text-error">
          <v-icon icon="mdi-alert" class="mr-2" />
          Confirm Factory Reset
        </v-card-title>
        <v-card-text>
          <v-alert type="error" variant="tonal" class="mb-4">
            <div class="text-subtitle-2 mb-2">This action is irreversible!</div>
            <div class="text-body-2">
              All device settings will be permanently erased, including:
            </div>
            <ul class="mt-2">
              <li>WiFi credentials</li>
              <li>Image processing settings</li>
              <li>Device configuration</li>
              <li>All custom settings</li>
            </ul>
          </v-alert>
          <div class="text-body-1 mb-3">
            The device will restart and return to factory defaults. Are you sure you want to
            continue?
          </div>
          <v-alert type="info" variant="tonal" density="compact">
            <div class="text-body-2">
              <strong>After reset:</strong> The device will create a WiFi access point named
              <strong>"PhotoFrame"</strong>. Connect to it from your device to restart the
              provisioning process.
            </div>
          </v-alert>
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="showFactoryResetDialog = false">Cancel</v-btn>
          <v-btn color="error" variant="flat" :loading="resetting" @click="performFactoryReset">
            Reset Device
          </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>

    <!-- Flash Mode Confirmation Dialog -->
    <v-dialog v-model="showFlashModeDialog" max-width="500">
      <v-card>
        <v-card-title>
          <v-icon icon="mdi-usb-flash-drive" class="mr-2" />
          Enter Flash Mode
        </v-card-title>
        <v-card-text>
          <div class="text-body-1 mb-3">
            The device will reboot into USB download mode and go offline. Then, from your computer,
            run your flash command (esptool) over USB — no BOOT/RST buttons needed.
          </div>
          <v-alert type="info" variant="tonal" density="compact">
            <div class="text-body-2">
              To cancel without flashing, just power-cycle the device (it boots normally on a full
              reset).
            </div>
          </v-alert>
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="showFlashModeDialog = false">Cancel</v-btn>
          <v-btn
            color="primary"
            variant="flat"
            :loading="enteringFlashMode"
            @click="enterFlashMode"
          >
            Enter Flash Mode
          </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>

    <!-- Import Config Confirmation Dialog -->
    <v-dialog v-model="showImportDialog" max-width="500">
      <v-card>
        <v-card-title>
          <v-icon icon="mdi-upload" class="mr-2" />
          Import Config
        </v-card-title>
        <v-card-text>
          <v-alert type="warning" variant="tonal" class="mb-4">
            This will overwrite your current settings with the imported config.
          </v-alert>
          <div class="text-body-2 mb-2">
            File: <strong>{{ importFileName }}</strong>
          </div>
          <div v-if="importData" class="text-body-2">
            Sections to import:
            <ul class="mt-1 ml-4">
              <li v-if="importData.config">Device settings</li>
              <li v-if="importData.processing">Processing settings</li>
              <li v-if="importData.palette">Palette calibration</li>
            </ul>
          </div>
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="showImportDialog = false">Cancel</v-btn>
          <v-btn color="primary" variant="flat" @click="performImport"> Import </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>
  </div>
</template>

<style scoped></style>
