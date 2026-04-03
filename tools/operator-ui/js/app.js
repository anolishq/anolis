/**
 * Main Application Entry Point
 */

import * as API from "./api.js";
import * as SSE from "./sse.js";
import * as UI from "./ui.js";
import * as Automation from "./automation.js";
import * as DeviceOverview from "./device-overview.js";
import * as DeviceDetail from "./device-detail.js";
import * as Telemetry from "./telemetry.js";
import { CONFIG } from "./config.js";

// DOM elements
const elements = {
  runtimeBadge: document.getElementById("runtime-badge"),
  connectionBadge: document.getElementById("connection-badge"),
  lastUpdate: document.getElementById("last-update"),
  lastError: document.getElementById("last-error"),
  errorBanner: document.getElementById("error-banner"),
  errorBannerMessage: document.getElementById("error-banner-message"),
  errorBannerDismiss: document.getElementById("error-banner-dismiss"),
  // Views
  dashboardView: document.getElementById("view-dashboard"),
  devicesView: document.getElementById("view-devices"),
  telemetryView: document.getElementById("view-telemetry"),
  // Tabs
  dashboardTab: document.getElementById("tab-dashboard"),
  devicesTab: document.getElementById("tab-devices"),
  telemetryTab: document.getElementById("tab-telemetry"),
};

let currentView = "dashboard";
let errorDismissTimeout = null;

/**
 * Show error banner
 */
function showErrorBanner(message, type = "error") {
  if (!elements.errorBanner || !elements.errorBannerMessage) return;

  elements.errorBannerMessage.textContent = message;
  elements.errorBanner.className = `error-banner ${type}`;
  elements.errorBanner.classList.remove("hidden");

  // Auto-dismiss after 10 seconds
  if (errorDismissTimeout) clearTimeout(errorDismissTimeout);
  errorDismissTimeout = setTimeout(() => {
    elements.errorBanner.classList.add("hidden");
  }, 10000);
}

/**
 * Hide error banner
 */
function hideErrorBanner() {
  if (!elements.errorBanner) return;
  elements.errorBanner.classList.add("hidden");
  if (errorDismissTimeout) clearTimeout(errorDismissTimeout);
}

/**
 * Switch between views
 */
function switchView(viewName) {
  if (viewName === currentView) return;

  currentView = viewName;

  // Hide all views
  elements.dashboardView.classList.add("view-hidden");
  elements.dashboardView.classList.remove("view-active");
  elements.devicesView.classList.add("view-hidden");
  elements.devicesView.classList.remove("view-active");
  elements.telemetryView.classList.add("view-hidden");
  elements.telemetryView.classList.remove("view-active");

  // Deactivate all tabs
  elements.dashboardTab.classList.remove("active");
  elements.devicesTab.classList.remove("active");
  elements.telemetryTab.classList.remove("active");

  // Activate selected view and tab
  if (viewName === "dashboard") {
    elements.dashboardView.classList.add("view-active");
    elements.dashboardView.classList.remove("view-hidden");
    elements.dashboardTab.classList.add("active");
  } else if (viewName === "devices") {
    elements.devicesView.classList.add("view-active");
    elements.devicesView.classList.remove("view-hidden");
    elements.devicesTab.classList.add("active");
  } else if (viewName === "telemetry") {
    elements.telemetryView.classList.add("view-active");
    elements.telemetryView.classList.remove("view-hidden");
    elements.telemetryTab.classList.add("active");

    // Activate telemetry module
    Telemetry.activate();
  }

  console.log(`[App] Switched to ${viewName} view`);
}

/**
 * Handle URL hash routing for device selection
 */
function handleRouting() {
  const hash = window.location.hash;

  if (!hash || hash === "#" || hash === "#dashboard") {
    switchView("dashboard");
  } else if (hash === "#telemetry") {
    switchView("telemetry");
  } else if (hash.startsWith("#devices")) {
    switchView("devices");

    // Parse device selection from hash: #devices/provider/device
    const parts = hash.split("/");
    if (parts.length === 3) {
      const provider = parts[1];
      const device = parts[2];
      // Delay to ensure view is loaded
      setTimeout(() => DeviceDetail.selectDevice(provider, device), 100);
    }
  }
}

/**
 * Initialize application
 */
async function init() {
  // Allow runtime API base override via ?api= query parameter.
  // Used by system-composer to point operator-ui at the correct runtime port.
  const params = new URLSearchParams(window.location.search);
  const apiOverride = params.get('api');
  if (apiOverride) {
    CONFIG.API_BASE = apiOverride;
    console.log(`[App] API base overridden via URL param: ${CONFIG.API_BASE}`);
  }

  console.log("Anolis Control Dashboard starting...");

  // Initialize modules
  Automation.init({
    automationSection: "automation-section",
    modeDisplay: "mode-display",
    modeSelector: "mode-selector",
    setModeButton: "set-mode-button",
    modeFeedback: "mode-feedback",
    parametersContainer: "parameters-container",
    btViewer: "bt-viewer",
    eventList: "event-list",
  });

  DeviceOverview.init({
    container: "devices-container",
  });

  DeviceDetail.init({
    selector: "device-selector",
    detailPanel: "device-detail-container",
  });

  Telemetry.init({
    iframe: "grafana-iframe",
  });

  // Setup periodic device state synchronization
  setInterval(() => {
    const deviceStates = DeviceDetail.getDeviceStates();
    DeviceOverview.render(deviceStates);
  }, 2000);

  // Setup tab navigation
  elements.dashboardTab.addEventListener("click", () => {
    window.location.hash = "#dashboard";
  });
  elements.devicesTab.addEventListener("click", () => {
    window.location.hash = "#devices";
  });
  elements.telemetryTab.addEventListener("click", () => {
    window.location.hash = "#telemetry";
  });

  // Setup hash change listener for routing
  window.addEventListener("hashchange", handleRouting);

  // Handle initial route
  handleRouting();

  // Make updateParameter available globally for inline handlers
  window.updateParameter = Automation.updateParameter;

  // SSE connection status handler
  SSE.on("connection_status", (status) => {
    // Handle new object-based status
    if (typeof status === "object") {
      const { state, attempts, delay_ms, idleTime } = status;

      if (state === "connected") {
        UI.updateBadge(elements.connectionBadge, "connected");
        elements.connectionBadge.textContent = "Connected";
      } else if (state === "reconnecting") {
        UI.updateBadge(elements.connectionBadge, "connecting");
        const delaySec = Math.ceil(delay_ms / 1000);
        elements.connectionBadge.textContent = `Reconnecting (${delaySec}s)`;
      } else if (state === "disconnected") {
        UI.updateBadge(elements.connectionBadge, "disconnected");
        elements.connectionBadge.textContent = "Disconnected";
        if (attempts > 0) {
          showErrorBanner(
            "Connection to runtime lost. Reconnecting...",
            "warning",
          );
        }
      } else if (state === "stale") {
        UI.updateBadge(elements.connectionBadge, "stale");
        const idleSec = Math.floor(idleTime / 1000);
        elements.connectionBadge.textContent = `Stale (${idleSec}s)`;
      }
    } else {
      // Legacy string-based status
      UI.updateBadge(elements.connectionBadge, status);
    }
  });

  // Provider health change handler
  SSE.on("provider_health_change", (data) => {
    console.log("[App] Provider health change:", data);
    if (data.state === "UNAVAILABLE") {
      showErrorBanner(`Provider ${data.provider_id} disconnected`, "warning");
    } else if (data.state === "AVAILABLE") {
      showErrorBanner(`Provider ${data.provider_id} reconnected`, "info");
    }
  });

  // Error banner dismiss button
  if (elements.errorBannerDismiss) {
    elements.errorBannerDismiss.addEventListener("click", hideErrorBanner);
  }

  // Start SSE connection
  SSE.connect();

  // Check runtime status
  await checkRuntimeStatus();
  setInterval(checkRuntimeStatus, 5000);

  // Periodic timestamp update
  setInterval(() => {
    UI.updateTimestamp(elements.lastUpdate);
  }, 1000);
}

/**
 * Check runtime status
 */
async function checkRuntimeStatus() {
  try {
    const data = await API.fetchRuntimeStatus();
    const status = data.status?.code === "OK" ? "ok" : "unavailable";
    UI.updateBadge(elements.runtimeBadge, status);
    UI.clearError(elements.lastError);
  } catch (err) {
    UI.updateBadge(elements.runtimeBadge, "unavailable");
    UI.showError(`Runtime error: ${err.message}`, elements.lastError);
    showErrorBanner(`Runtime API error: ${err.message}`, "error");
  }
}

// Start the application
init();
