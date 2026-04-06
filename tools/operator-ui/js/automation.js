/**
 * Automation Module - Mode, parameters, BT, events
 */

import * as API from "./api.js";
import * as SSE from "./sse.js";
import * as UI from "./ui.js";
import { CONFIG, AUTOMATION_MODES } from "./config.js";

let elements = {};
let currentMode = null;
let eventBuffer = [];
let modeSelectorDirty = false; // Track if user has manually changed dropdown

/**
 * Initialize automation module
 */
export function init(elementIds) {
  elements = {
    automationSection: document.getElementById(elementIds.automationSection),
    modeDisplay: document.getElementById(elementIds.modeDisplay),
    modeSelector: document.getElementById(elementIds.modeSelector),
    setModeButton: document.getElementById(elementIds.setModeButton),
    modeFeedback: document.getElementById(elementIds.modeFeedback),
    parametersContainer: document.getElementById(
      elementIds.parametersContainer,
    ),
    btViewer: document.getElementById(elementIds.btViewer),
    eventList: document.getElementById(elementIds.eventList),
    // Health display elements
    btStatus: document.getElementById("bt-status"),
    btTotalTicks: document.getElementById("bt-total-ticks"),
    btTicksSinceProgress: document.getElementById("bt-ticks-since-progress"),
    btErrorCount: document.getElementById("bt-error-count"),
    btLastError: document.getElementById("bt-last-error"),
  };

  // Event listeners
  elements.setModeButton.addEventListener("click", handleSetMode);

  // Track when user manually changes dropdown (dirty state)
  elements.modeSelector.addEventListener("change", () => {
    modeSelectorDirty = true;
  });

  // SSE event handlers
  SSE.on("mode_change", handleModeChange);
  SSE.on("parameter_change", handleParameterChange);
  SSE.on("bt_error", handleBTError);

  // Initial load
  refreshAll();

  // Periodic refresh
  setInterval(refreshAll, 5000);
}

/**
 * Refresh all automation UI
 */
async function refreshAll() {
  await Promise.all([
    refreshMode(),
    refreshParameters(),
    refreshAutomationHealth(),
    loadBehaviorTree(),
  ]);
}

/**
 * Refresh mode display
 */
async function refreshMode() {
  try {
    const data = await API.fetchMode();
    if (data.status?.code === "OK") {
      currentMode = data.mode;
      elements.modeDisplay.textContent = currentMode;
      elements.modeDisplay.className = `badge ${currentMode.toLowerCase()}`;

      // Only update dropdown if user hasn't manually changed it
      if (!modeSelectorDirty) {
        elements.modeSelector.value = currentMode;
      }

      UI.show(elements.automationSection);
    }
  } catch (err) {
    console.error("Failed to fetch mode:", err);
  }
}

/**
 * Handle mode change from SSE
 */
function handleModeChange(data) {
  currentMode = data.new_mode;
  elements.modeDisplay.textContent = currentMode;
  elements.modeDisplay.className = `badge ${currentMode.toLowerCase()}`;

  // Only update dropdown if user hasn't manually changed it
  if (!modeSelectorDirty) {
    elements.modeSelector.value = currentMode;
  }

  addEvent(
    "mode_change",
    `${data.previous_mode} -> ${data.new_mode}`,
    data.timestamp_ms,
  );
}

/**
 * Refresh automation health display
 */
async function refreshAutomationHealth() {
  try {
    const data = await API.fetchAutomationStatus();

    if (data.status?.code === "OK") {
      // Update BT status badge
      const btStatus = data.bt_status || "UNKNOWN";
      elements.btStatus.textContent = btStatus;
      elements.btStatus.className = `badge bt-${btStatus.toLowerCase()}`;

      // Update metrics
      elements.btTotalTicks.textContent = data.total_ticks || 0;
      elements.btTicksSinceProgress.textContent =
        data.ticks_since_progress || 0;
      elements.btErrorCount.textContent = data.error_count || 0;
      elements.btLastError.textContent = data.last_error || "--";

      // Apply semantic style to last error based on BT health
      if (btStatus === "STALLED") {
        elements.btLastError.className = "error-text warning";
      } else if (btStatus === "ERROR") {
        elements.btLastError.className = "error-text alarm";
      } else {
        elements.btLastError.className = "error-text";
      }
    }
  } catch (err) {
    // Automation might not be enabled, fail silently
    console.debug("Automation health not available:", err);
  }
}

/**
 * Handle BT error event from SSE
 */
function handleBTError(data) {
  // Update error display
  elements.btLastError.textContent = data.error;
  elements.btLastError.className = "error-text alarm";

  // Increment error count
  const currentCount = parseInt(elements.btErrorCount.textContent) || 0;
  elements.btErrorCount.textContent = currentCount + 1;

  // Update status badge to ERROR
  elements.btStatus.textContent = "ERROR";
  elements.btStatus.className = "badge bt-error";

  // Add to event trace
  const errorMsg = data.node ? `${data.node}: ${data.error}` : data.error;
  addEvent("bt_error", errorMsg, data.timestamp_ms);
}

/**
 * Handle set mode button click
 */
async function handleSetMode() {
  const newMode = elements.modeSelector.value;

  elements.modeFeedback.className = "feedback pending";
  elements.modeFeedback.textContent = "Setting...";

  try {
    const result = await API.setMode(newMode);

    if (result.status?.code === "OK") {
      elements.modeFeedback.textContent = "Mode set";
      elements.modeFeedback.className = "feedback success";

      // Reset dirty flag - mode change successful
      modeSelectorDirty = false;

      setTimeout(() => {
        elements.modeFeedback.textContent = "";
        elements.modeFeedback.className = "feedback";
      }, 2000);
    } else {
      throw new Error(result.status?.message || "Failed to set mode");
    }
  } catch (err) {
    elements.modeFeedback.textContent = `Error: ${err.message}`;
    elements.modeFeedback.className = "feedback error";
    console.error("Failed to set mode:", err);
  }
}

/**
 * Refresh parameters display
 */
async function refreshParameters() {
  try {
    const parameters = await API.fetchParameters();

    if (parameters.length === 0) {
      elements.parametersContainer.innerHTML =
        '<p class="placeholder">No parameters available</p>';
      return;
    }

    let html = '<div class="parameters-grid">';
    for (const param of parameters) {
      html += `
        <div class="parameter-card">
          <div class="parameter-header">
            <span class="parameter-name">${UI.escapeHtml(param.name)}</span>
            <span class="parameter-type">${UI.escapeHtml(param.type)}</span>
          </div>
          <div class="parameter-value">
            <strong>${UI.escapeHtml(String(param.value))}</strong>
          </div>
          <div class="parameter-controls">
            <input type="text" 
                   id="param-${UI.escapeHtml(param.name)}" 
                   value="${UI.escapeHtml(String(param.value))}"
                   placeholder="New value">
            <button onclick="window.updateParameter('${UI.escapeHtml(param.name)}', '${UI.escapeHtml(param.type)}')">Set</button>
            <span id="param-feedback-${UI.escapeHtml(param.name)}" class="feedback"></span>
          </div>
          ${param.min !== undefined || param.max !== undefined ? `<div class="parameter-range">Range: [${param.min}, ${param.max}]</div>` : ""}
        </div>
      `;
    }
    html += "</div>";
    elements.parametersContainer.innerHTML = html;
  } catch (err) {
    console.error("Failed to refresh parameters:", err);
  }
}

/**
 * Update parameter value
 */
export async function updateParameter(name, type) {
  const inputElement = document.getElementById(`param-${name}`);
  const feedbackElement = document.getElementById(`param-feedback-${name}`);
  const rawValue = inputElement.value;

  try {
    // Parse value based on type
    let value;
    if (type === "int64" || type === "uint64") {
      value = parseInt(rawValue, 10);
      if (isNaN(value)) throw new Error("Invalid integer");
    } else if (type === "double") {
      value = parseFloat(rawValue);
      if (isNaN(value)) throw new Error("Invalid number");
    } else if (type === "bool") {
      value = rawValue.toLowerCase() === "true";
    } else {
      value = rawValue;
    }

    const result = await API.updateParameter(name, value);

    if (result.status?.code === "OK") {
      feedbackElement.textContent = "Updated";
      feedbackElement.className = "feedback success";
      setTimeout(() => {
        feedbackElement.textContent = "";
      }, 2000);
      await refreshParameters();
    } else {
      throw new Error(result.status?.message || "Update failed");
    }
  } catch (err) {
    feedbackElement.textContent = `Error: ${err.message}`;
    feedbackElement.className = "feedback error";
  }
}

/**
 * Handle parameter change from SSE
 */
function handleParameterChange(data) {
  addEvent(
    "parameter_change",
    `${data.parameter_name}: ${data.old_value} -> ${data.new_value}`,
    data.timestamp_ms,
  );
  refreshParameters();
}

/**
 * Load and display behavior tree
 */
async function loadBehaviorTree() {
  try {
    const treeXml = await API.fetchBehaviorTree();
    if (!treeXml) {
      elements.btViewer.textContent = "No behavior tree loaded";
      return;
    }

    // Parse and render as text outline
    const parser = new DOMParser();
    const xmlDoc = parser.parseFromString(treeXml, "text/xml");
    const outline = renderBTOutline(xmlDoc);
    elements.btViewer.textContent = outline;
  } catch (err) {
    elements.btViewer.textContent = `Error: ${err.message}`;
    console.error("Failed to load BT:", err);
  }
}

/**
 * Render BT as text outline
 */
function renderBTOutline(xmlDoc, node = null, indent = 0, isLast = true) {
  if (!node) {
    const root = xmlDoc.querySelector("BehaviorTree");
    if (!root) return "No BehaviorTree found";
    return renderBTOutline(xmlDoc, root, 0, true);
  }

  let output = "";
  const prefix =
    indent === 0
      ? ""
      : " ".repeat((indent - 1) * 2) + (isLast ? "\\- " : "|- ");
  const nodeName = node.getAttribute("name") || "";
  output += `${prefix}${node.tagName}${nodeName ? ` "${nodeName}"` : ""}\n`;

  const children = Array.from(node.children);
  for (let i = 0; i < children.length; i++) {
    output += renderBTOutline(
      xmlDoc,
      children[i],
      indent + 1,
      i === children.length - 1,
    );
  }

  return output;
}

/**
 * Add event to trace
 */
function addEvent(eventType, details, timestampMs) {
  const timestampValue = Number(timestampMs);
  const timestamp =
    Number.isFinite(timestampValue) && timestampValue > 0
      ? new Date(timestampValue).toLocaleTimeString()
      : new Date().toLocaleTimeString();
  eventBuffer.push({ type: eventType, details, timestamp });

  if (eventBuffer.length > CONFIG.MAX_EVENTS) {
    eventBuffer.shift();
  }

  renderEvents();
}

/**
 * Render event trace
 */
function renderEvents() {
  if (eventBuffer.length === 0) {
    elements.eventList.innerHTML = '<p class="placeholder">No events yet</p>';
    return;
  }

  let html = '<div class="event-items">';
  // Show most recent first
  for (let i = eventBuffer.length - 1; i >= 0; i--) {
    const event = eventBuffer[i];
    html += `
      <div class="event-item ${event.type.replace("_", "-")}">
        <span class="event-time">${event.timestamp}</span>
        <span class="event-type">${event.type}</span>
        <span class="event-details">${UI.escapeHtml(event.details)}</span>
      </div>
    `;
  }
  html += "</div>";
  elements.eventList.innerHTML = html;
}
