/**
 * Configuration constants for Anolis Operator UI
 */

export const CONFIG = {
  API_BASE: "http://localhost:8080",
  POLL_INTERVAL_MS: 500,
  SSE_RECONNECT_DELAY_MS: 3000,
  MAX_EVENTS: 100,
  DEFAULT_THEME: "dark",
  THEME_STORAGE_KEY: "anolis.operator_ui.theme",
};

export const THEMES = ["dark", "light"];

export const AUTOMATION_MODES = ["MANUAL", "AUTO", "IDLE"];

export const QUALITY_LEVELS = [
  "OK",
  "STALE",
  "UNAVAILABLE",
  "FAULT",
  "UNKNOWN",
];
