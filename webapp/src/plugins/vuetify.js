import "vuetify/styles";
import { createVuetify } from "vuetify";

// Selectable themes (light + dark for a few color schemes). The header lets the
// user pick one; the choice is persisted to localStorage. Keep this list in sync
// with the theme menu in AppHeader.vue.
export const themeOptions = [
  { value: "terracotta", title: "Terracotta (Light)" },
  { value: "terracottaDark", title: "Terracotta (Dark)" },
  { value: "ocean", title: "Ocean (Light)" },
  { value: "oceanDark", title: "Ocean (Dark)" },
  { value: "forest", title: "Forest (Light)" },
  { value: "forestDark", title: "Forest (Dark)" },
];

export const THEME_STORAGE_KEY = "pf_theme";
export const DEFAULT_THEME = "terracotta";

const lightTheme = (primary) => ({
  dark: false,
  colors: {
    primary,
    secondary: "#424242",
    accent: "#82B1FF",
    error: "#982f2f",
    info: "#2f6398",
    success: "#2f9852",
    warning: "#987e2f",
    background: "#F5F5F5",
    surface: "#FFFFFF",
  },
});

const darkTheme = (primary) => ({
  dark: true,
  colors: {
    primary,
    secondary: "#9e9e9e",
    accent: "#82B1FF",
    error: "#cf6679",
    info: "#4a90d9",
    success: "#3fae68",
    warning: "#c9a227",
    background: "#121212",
    surface: "#1e1e1e",
  },
});

export default createVuetify({
  theme: {
    defaultTheme: DEFAULT_THEME,
    themes: {
      terracotta: lightTheme("#ce9160"),
      terracottaDark: darkTheme("#ce9160"),
      ocean: lightTheme("#2f6398"),
      oceanDark: darkTheme("#4a90d9"),
      forest: lightTheme("#2f9852"),
      forestDark: darkTheme("#3fae68"),
    },
  },
  defaults: {
    VBtn: {
      variant: "flat",
    },
    VCard: {
      elevation: 2,
    },
  },
});
