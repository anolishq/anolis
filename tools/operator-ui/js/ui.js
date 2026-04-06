/**
 * UI Utilities - Shared DOM manipulation and formatting
 */

import { QUALITY_LEVELS } from "./config.js";

/**
 * Escape HTML to prevent XSS
 */
export function escapeHtml(text) {
  if (text == null) return "";
  const div = document.createElement("div");
  div.textContent = String(text);
  return div.innerHTML;
}

/**
 * Format signal value for display
 */
export function formatValue(value) {
  if (!value) return "--";

  const type = value.type;
  if (type === "double" && value.double !== undefined) {
    return value.double.toFixed(4);
  } else if (type === "int64" && value.int64 !== undefined) {
    return String(value.int64);
  } else if (type === "uint64" && value.uint64 !== undefined) {
    return String(value.uint64);
  } else if (type === "bool" && value.bool !== undefined) {
    return value.bool ? "true" : "false";
  } else if (type === "string" && value.string !== undefined) {
    return value.string;
  }
  return JSON.stringify(value);
}

/**
 * Validate and normalize quality level
 */
export function normalizeQuality(quality) {
  return QUALITY_LEVELS.includes(quality) ? quality : "UNKNOWN";
}

/**
 * Update badge element
 */
export function updateBadge(element, status) {
  element.textContent = status.toUpperCase();
  element.className = `badge ${status.toLowerCase()}`;
}

/**
 * Show/hide element
 */
export function show(element) {
  element.classList.remove("hidden");
}

export function hide(element) {
  element.classList.add("hidden");
}

/**
 * Show error message
 */
export function showError(message, container) {
  container.textContent = message;
  show(container);
}

/**
 * Clear error message
 */
export function clearError(container) {
  hide(container);
}

/**
 * Update timestamp display
 */
export function updateTimestamp(element) {
  const now = new Date();
  element.textContent = `Last update: ${now.toLocaleTimeString()}`;
}
