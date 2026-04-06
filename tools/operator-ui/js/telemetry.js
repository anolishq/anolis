/**
 * Telemetry Module - Grafana dashboard integration with tab navigation
 */

let elements = {};

/**
 * Initialize telemetry module
 */
export function init(elementIds) {
  elements = {
    iframe: document.getElementById(elementIds.iframe),
  };

  console.log("[Telemetry] Module initialized");
}

/**
 * Load Grafana when telemetry view is activated
 */
export function activate() {
  console.log("[Telemetry] Activating view...");

  // Load Grafana URL when view becomes active (lazy load)
  if (elements.iframe.src === "about:blank") {
    elements.iframe.src = "http://localhost:3001";
    console.log("[Telemetry] Loading Grafana iframe...");
  }
}

/**
 * Cleanup when switching away from telemetry view
 */
export function deactivate() {
  // Could pause iframe or clear memory if needed
  console.log("[Telemetry] Deactivating view");
}
