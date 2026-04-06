/**
 * Device Detail Module - Device selection and detailed view
 * Shows full state, parameters, and functions for selected device
 */

import * as API from "./api.js";
import * as SSE from "./sse.js";
import * as UI from "./ui.js";

let elements = {};
let allDevices = []; // Full device list from API
let deviceStates = {}; // Map of "provider/device" -> {device, signals}
let deviceCapabilities = {}; // Map of "provider/device" -> normalized capabilities
let selectedDevice = null; // {provider_id, device_id, device}
const INT64_MIN = -9223372036854775808n;
const INT64_MAX = 9223372036854775807n;
const UINT64_MAX = 18446744073709551615n;
const JS_SAFE_MIN = BigInt(Number.MIN_SAFE_INTEGER);
const JS_SAFE_MAX = BigInt(Number.MAX_SAFE_INTEGER);

/**
 * Initialize device detail module
 */
export function init(elementIds) {
  elements = {
    selector: document.getElementById(elementIds.selector),
    detailPanel: document.getElementById(elementIds.detailPanel),
  };

  // SSE event handlers for real-time updates
  SSE.on("state_update", handleStateUpdate);
  SSE.on("quality_change", handleQualityChange);

  // Device selector change handler
  elements.selector.addEventListener("change", (e) => {
    const [provider, device] = e.target.value.split("/");
    if (provider && device) {
      selectDevice(provider, device);
    }
  });

  // Initial load
  loadDevices();

  console.log("[DeviceDetail] Module initialized");
}

/**
 * Load all devices and populate selector
 */
async function loadDevices() {
  try {
    allDevices = await API.fetchDevices();

    // Fetch state for each device
    for (const device of allDevices) {
      try {
        const state = await API.fetchDeviceState(
          device.provider_id,
          device.device_id,
        );
        deviceStates[`${device.provider_id}/${device.device_id}`] = {
          device,
          signals: state,
        };
      } catch (err) {
        console.error(
          `Failed to fetch state for ${device.provider_id}/${device.device_id}:`,
          err,
        );
      }
    }

    populateSelector();
  } catch (err) {
    console.error("Failed to load devices:", err);
    elements.selector.innerHTML = "<option>Failed to load devices</option>";
  }
}

/**
 * Group devices by type and populate selector dropdown
 */
function populateSelector() {
  const grouped = groupDevicesByType(allDevices);

  let html = '<option value="">Select a device...</option>';

  // Sort type keys alphabetically
  const types = Object.keys(grouped).sort();

  for (const type of types) {
    const devices = grouped[type];
    const typeLabel = type === "unknown" ? "Unknown Type" : type.toUpperCase();

    html += `<optgroup label="${UI.escapeHtml(typeLabel)}">`;

    for (const device of devices) {
      const value = `${device.provider_id}/${device.device_id}`;
      const selected =
        selectedDevice &&
        selectedDevice.provider_id === device.provider_id &&
        selectedDevice.device_id === device.device_id
          ? "selected"
          : "";

      html += `<option value="${UI.escapeHtml(value)}" ${selected}>
        ${UI.escapeHtml(device.display_name || device.device_id)}
      </option>`;
    }

    html += "</optgroup>";
  }

  elements.selector.innerHTML = html;
}

/**
 * Group devices by type field
 */
function groupDevicesByType(devices) {
  const grouped = {};

  devices.forEach((dev) => {
    const type = dev.type || "unknown";
    if (!grouped[type]) {
      grouped[type] = [];
    }
    grouped[type].push(dev);
  });

  // Sort devices within each type by device_id
  Object.keys(grouped).forEach((type) => {
    grouped[type].sort((a, b) => a.device_id.localeCompare(b.device_id));
  });

  return grouped;
}

/**
 * Select and display a specific device
 */
export async function selectDevice(provider, device) {
  const key = `${provider}/${device}`;
  let state = deviceStates[key];

  // If device not in cache, try to fetch it
  if (!state) {
    console.warn(`Device ${key} not in cache, attempting to fetch...`);
    elements.detailPanel.innerHTML =
      '<p class="placeholder">Loading device state...</p>';

    // Find device info from allDevices list
    const deviceInfo = allDevices.find(
      (d) => d.provider_id === provider && d.device_id === device,
    );
    if (!deviceInfo) {
      elements.detailPanel.innerHTML = `
        <div class="error-message">
          <h3>Device Not Found</h3>
          <p>Device <code>${provider}/${device}</code> does not exist.</p>
        </div>
      `;
      return;
    }

    // Try to fetch state
    try {
      const signals = await API.fetchDeviceState(provider, device);
      state = { device: deviceInfo, signals };
      deviceStates[key] = state; // Add to cache
      console.log(`[DeviceDetail] Successfully loaded ${key}`);
    } catch (err) {
      // Check if this is a device with no signals (functions-only device)
      if (err.message.includes("Device state not available")) {
        console.log(
          `[DeviceDetail] ${key} has no signals (functions-only device)`,
        );
        state = { device: deviceInfo, signals: [] }; // Empty signals array
        deviceStates[key] = state; // Add to cache
      } else {
        // Real error - device is actually unavailable
        elements.detailPanel.innerHTML = `
          <div class="error-message">
            <h3>Device Unavailable</h3>
            <p>Could not load state for <code>${provider}/${device}</code></p>
            <p class="error-details">${err.message}</p>
            <button class="btn-primary" onclick="location.reload()">Retry</button>
          </div>
        `;
        console.error(`[DeviceDetail] Failed to load ${key}:`, err);
        return;
      }
    }
  }

  selectedDevice = {
    provider_id: provider,
    device_id: device,
    device: state.device,
  };

  // Update selector to match
  elements.selector.value = key;

  // Update URL hash
  if (window.location.hash !== `#devices/${provider}/${device}`) {
    window.location.hash = `#devices/${provider}/${device}`;
  }

  // Render device detail
  renderDeviceDetail(state);

  // Render cached capabilities immediately if present, then refresh from API
  const capabilityKey = `${provider}/${device}`;
  if (deviceCapabilities[capabilityKey]) {
    renderFunctions(deviceCapabilities[capabilityKey]);
  }
  loadDeviceCapabilities(provider, device);

  console.log(`[DeviceDetail] Selected device: ${key}`);
}

/**
 * Render full device detail view
 */
function renderDeviceDetail(state) {
  const { device, signals } = state;
  const quality = getAggregateQuality(signals);

  let html = `
    <div class="device-detail-header">
      <div class="device-detail-title">
        <h2>${UI.escapeHtml(device.display_name || device.device_id)}</h2>
        <span class="device-type-badge">${UI.escapeHtml(device.type || "unknown")}</span>
        <span class="badge ${quality.toLowerCase()}">${quality}</span>
      </div>
      <div class="device-detail-meta">
        <span class="device-id">Device: ${UI.escapeHtml(device.device_id)}</span>
        <span class="device-provider">Provider: ${UI.escapeHtml(device.provider_id)}</span>
      </div>
    </div>
    
    <div class="device-detail-section">
      <h3>Signals</h3>
      <div class="state-table-full">
  `;

  if (signals.length === 0) {
    html +=
      '<p class="placeholder">This device has no signals (functions-only device)</p>';
  } else {
    html +=
      "<table><thead><tr><th>Signal</th><th>Value</th><th>Quality</th><th>Timestamp</th></tr></thead><tbody>";

    for (const signal of signals) {
      const quality = UI.normalizeQuality(signal.quality);
      const value = UI.formatValue(signal.value);
      const timestamp =
        signal.timestamp_ms && signal.timestamp_ms > 0
          ? new Date(signal.timestamp_ms).toLocaleTimeString()
          : "--";

      html += `
        <tr>
          <td class="signal-name">${UI.escapeHtml(signal.signal_id)}</td>
          <td class="signal-value">${UI.escapeHtml(value)}</td>
          <td><span class="badge ${quality.toLowerCase()}">${quality}</span></td>
          <td class="signal-timestamp">${timestamp}</td>
        </tr>
      `;
    }

    html += "</tbody></table>";
  }

  html += "</div></div>";

  // Functions section (placeholder, will be populated by loadDeviceCapabilities)
  html += `
    <div class="device-detail-section">
      <h3>Functions</h3>
      <div id="device-functions-container">
        <p class="placeholder">Loading capabilities...</p>
      </div>
    </div>
  `;

  elements.detailPanel.innerHTML = html;
  renderCachedFunctionsForSelected();
}

/**
 * Get aggregate quality from all signals
 */
function getAggregateQuality(signals) {
  if (signals.length === 0) return "OK"; // No signals = no problems

  const qualities = signals.map((s) => UI.normalizeQuality(s.quality));

  if (qualities.some((q) => q === "FAULT")) return "FAULT";
  if (qualities.some((q) => q === "UNAVAILABLE")) return "UNAVAILABLE";
  if (qualities.some((q) => q === "STALE")) return "STALE";
  if (qualities.every((q) => q === "OK")) return "OK";

  return "UNKNOWN";
}

/**
 * Load device capabilities and render functions
 */
async function loadDeviceCapabilities(provider, device) {
  const capabilityKey = `${provider}/${device}`;
  try {
    const capabilities = await API.fetchDeviceCapabilities(provider, device);
    const normalized = normalizeCapabilities(capabilities);
    deviceCapabilities[capabilityKey] = normalized;

    if (
      selectedDevice &&
      selectedDevice.provider_id === provider &&
      selectedDevice.device_id === device
    ) {
      renderFunctions(normalized);
    }
  } catch (err) {
    console.error(
      `Failed to load capabilities for ${provider}/${device}:`,
      err,
    );
    // Keep last-known-good cache on transient failures.
    if (
      selectedDevice &&
      selectedDevice.provider_id === provider &&
      selectedDevice.device_id === device
    ) {
      const container = document.getElementById("device-functions-container");
      if (container && !deviceCapabilities[capabilityKey]) {
        container.innerHTML =
          '<p class="placeholder">No functions available</p>';
      }
    }
  }
}

function getSelectedDeviceKey() {
  if (!selectedDevice) return "";
  return `${selectedDevice.provider_id}/${selectedDevice.device_id}`;
}

function renderCachedFunctionsForSelected() {
  const key = getSelectedDeviceKey();
  if (!key) return;
  const cached = deviceCapabilities[key];
  if (cached) {
    renderFunctions(cached);
  }
}

function normalizeCapabilities(capabilities) {
  const normalized = capabilities ? { ...capabilities } : {};
  normalized.functions = normalizeFunctions(capabilities?.functions);
  return normalized;
}

function normalizeFunctions(functions) {
  if (!Array.isArray(functions)) return [];

  const normalized = functions.map((func, index) => {
    const functionId = Number(func?.function_id);
    const fallbackId = Number.isFinite(functionId) ? functionId : index + 1;
    const name =
      typeof func?.name === "string" && func.name.trim() !== ""
        ? func.name.trim()
        : typeof func?.function_name === "string" &&
            func.function_name.trim() !== ""
          ? func.function_name.trim()
          : `Function ${fallbackId}`;
    const description =
      typeof func?.label === "string" && func.label.trim() !== ""
        ? func.label.trim()
        : typeof func?.description === "string" &&
            func.description.trim() !== ""
          ? func.description.trim()
          : "";

    return {
      ...func,
      function_id: fallbackId,
      name,
      function_name: name,
      label: description,
      description,
      display_name: name,
      args: normalizeFunctionArgs(func?.args),
    };
  });

  normalized.sort((a, b) => {
    if (a.function_id !== b.function_id) {
      return a.function_id - b.function_id;
    }
    return a.display_name.localeCompare(b.display_name);
  });

  return normalized;
}

function normalizeFunctionArgs(args) {
  if (Array.isArray(args)) {
    return args
      .map((arg, index) => normalizeArgSpec(arg, `arg_${index + 1}`))
      .filter((arg) => arg.name !== "");
  }

  if (args && typeof args === "object") {
    return Object.entries(args)
      .map(([name, spec]) => normalizeArgSpec(spec, name))
      .filter((arg) => arg.name !== "")
      .sort((a, b) => a.name.localeCompare(b.name));
  }

  return [];
}

function normalizeArgSpec(arg, fallbackName = "") {
  const name =
    typeof arg?.name === "string" && arg.name.trim() !== ""
      ? arg.name.trim()
      : fallbackName;
  if (typeof name !== "string" || name.trim() === "") {
    return { name: "", type: "string", required: true };
  }

  return {
    name: name.trim(),
    type:
      typeof arg?.type === "string" && arg.type.trim() !== ""
        ? arg.type.trim()
        : "string",
    required: arg?.required !== false,
    min: arg?.min,
    max: arg?.max,
    description:
      typeof arg?.description === "string" ? arg.description.trim() : "",
    unit: typeof arg?.unit === "string" ? arg.unit.trim() : "",
  };
}

/**
 * Render device functions
 */
function renderFunctions(capabilities) {
  const container = document.getElementById("device-functions-container");
  if (!container) return;

  // Extract functions from capabilities
  const functions = capabilities.functions || [];

  if (functions.length === 0) {
    container.innerHTML = '<p class="placeholder">No functions available</p>';
    return;
  }

  let html = '<div class="functions-list">';

  for (const func of functions) {
    html += renderFunctionForm(func);
  }

  html += "</div>";
  container.innerHTML = html;

  // Attach event listeners to all forms
  container.querySelectorAll(".function-form").forEach((form) => {
    form.addEventListener("submit", handleFunctionSubmit);
  });
}

/**
 * Render a single function as an executable form
 */
function renderFunctionForm(func) {
  const formId = `func-${func.function_id}`;
  const functionName =
    func.display_name ||
    func.name ||
    func.function_name ||
    `Function ${func.function_id}`;
  const functionDescription = func.description || func.label || "";

  let html = `
    <div class="function-card">
      <form class="function-form" id="${formId}" data-function-id="${func.function_id}" data-function-name="${UI.escapeHtml(functionName)}">
        <h4>${UI.escapeHtml(functionName)}</h4>
  `;

  if (functionDescription) {
    html += `<p class="function-description">${UI.escapeHtml(functionDescription)}</p>`;
  }

  // Render arguments
  if (Array.isArray(func.args) && func.args.length > 0) {
    html += '<div class="function-args">';

    for (const [argIndex, arg] of func.args.entries()) {
      html += renderArgInput(arg, formId, argIndex);
    }

    html += "</div>";
  }

  html += `
        <div class="function-actions">
          <button type="submit" class="btn-primary">Execute</button>
          <span class="function-feedback"></span>
        </div>
      </form>
    </div>
  `;

  return html;
}

/**
 * Render an input field for a function argument
 */
function renderArgInput(arg, formId = "func", argIndex = 0) {
  const argId = `${formId}-arg-${argIndex}`;
  const argInputName = `arg_${argIndex}`;
  const required = arg.required !== false; // Default to required
  const requiredAttr = required ? "required" : "";
  const requiredLabel = required ? ' <span class="required">*</span>' : "";
  const unitLabel = arg.unit ? ` (${UI.escapeHtml(arg.unit)})` : "";

  let input = "";
  const type = arg.type || "string";

  switch (type) {
    case "double":
      input = `<input type="number" id="${argId}" name="${argInputName}" step="any" ${requiredAttr}`;
      if (arg.min !== undefined) input += ` min="${arg.min}"`;
      if (arg.max !== undefined) input += ` max="${arg.max}"`;
      input += ">";
      break;

    case "int64":
    case "uint64":
      // Keep as text so 64-bit values are not silently rounded by browser number inputs.
      input = `<input type="text" id="${argId}" name="${argInputName}" inputmode="numeric" ${requiredAttr}`;
      input += ">";
      break;

    case "bool":
      // For checkboxes, HTML "required" means "must be checked" (wrong semantics for bool args).
      input = `<input type="checkbox" id="${argId}" name="${argInputName}">`;
      break;

    case "bytes":
      input = `<input type="text" id="${argId}" name="${argInputName}" placeholder="Base64 encoded" ${requiredAttr}>`;
      break;

    case "string":
    default:
      input = `<input type="text" id="${argId}" name="${argInputName}" ${requiredAttr}>`;
      break;
  }

  // Add constraint hints
  let hint = "";
  if (arg.min !== undefined && arg.max !== undefined) {
    hint = ` <span class="constraint-hint">[${arg.min} - ${arg.max}]</span>`;
  } else if (arg.min !== undefined) {
    hint = ` <span class="constraint-hint">[min: ${arg.min}]</span>`;
  } else if (arg.max !== undefined) {
    hint = ` <span class="constraint-hint">[max: ${arg.max}]</span>`;
  }

  return `
    <div class="arg-row">
      <label for="${argId}">
        ${UI.escapeHtml(arg.name)}${unitLabel}${requiredLabel}${hint}
      </label>
      ${input}
    </div>
  `;
}

function parseIntegerArgument(value, argDef, valueType) {
  const argName = argDef.name;
  let parsedBigInt;
  try {
    parsedBigInt = BigInt(value);
  } catch (_err) {
    throw new Error(`Invalid ${valueType} argument: ${argName}`);
  }

  if (valueType === "int64") {
    if (parsedBigInt < INT64_MIN || parsedBigInt > INT64_MAX) {
      throw new Error(`Out-of-range int64 argument: ${argName}`);
    }
    if (parsedBigInt < JS_SAFE_MIN || parsedBigInt > JS_SAFE_MAX) {
      throw new Error(
        `int64 argument out of browser-safe range: ${argName} (use API for >53-bit values)`,
      );
    }
  } else {
    if (parsedBigInt < 0n || parsedBigInt > UINT64_MAX) {
      throw new Error(`Out-of-range uint64 argument: ${argName}`);
    }
    if (parsedBigInt > JS_SAFE_MAX) {
      throw new Error(
        `uint64 argument out of browser-safe range: ${argName} (use API for >53-bit values)`,
      );
    }
  }

  return Number(parsedBigInt);
}

/**
 * Handle function form submission
 */
async function handleFunctionSubmit(event) {
  event.preventDefault();

  if (!selectedDevice) {
    console.error("No device selected");
    return;
  }

  const form = event.target;
  const functionId = parseInt(form.dataset.functionId, 10);
  const functionName = form.dataset.functionName;
  const feedback = form.querySelector(".function-feedback");
  const submitBtn = form.querySelector('button[type="submit"]');

  // Disable button during execution
  submitBtn.disabled = true;
  feedback.textContent = "Executing...";
  feedback.className = "function-feedback executing";

  try {
    const selectedKey = getSelectedDeviceKey();
    const args = {};

    let capabilities = deviceCapabilities[selectedKey];
    if (!capabilities) {
      const rawCapabilities = await API.fetchDeviceCapabilities(
        selectedDevice.provider_id,
        selectedDevice.device_id,
      );
      capabilities = normalizeCapabilities(rawCapabilities);
      deviceCapabilities[selectedKey] = capabilities;
    }

    const funcDef = (capabilities.functions || []).find(
      (f) => f.function_id === functionId,
    );

    if (!funcDef) {
      throw new Error("Function definition not found");
    }

    const formData = new FormData(form);

    // Parse each argument according to its type
    for (const [argIndex, argDef] of (funcDef.args || []).entries()) {
      const inputName = `arg_${argIndex}`;
      const inputElement = form.elements.namedItem(inputName);
      const argType = argDef.type || "string";

      if (!inputElement) {
        if (argDef.required !== false) {
          throw new Error(`Missing required argument: ${argDef.name}`);
        }
        continue;
      }

      if (argType === "bool") {
        args[argDef.name] = {
          type: "bool",
          bool: Boolean(inputElement.checked),
        };
        continue;
      }

      const rawValue = formData.get(inputName);
      const value = typeof rawValue === "string" ? rawValue.trim() : "";
      if (value === "") {
        if (argDef.required !== false) {
          throw new Error(`Missing required argument: ${argDef.name}`);
        }
        continue;
      }

      switch (argType) {
        case "double": {
          const parsed = Number(value);
          if (Number.isNaN(parsed)) {
            throw new Error(`Invalid double argument: ${argDef.name}`);
          }
          args[argDef.name] = { type: "double", double: parsed };
          break;
        }
        case "int64": {
          const parsed = parseIntegerArgument(value, argDef, "int64");
          args[argDef.name] = { type: "int64", int64: parsed };
          break;
        }
        case "uint64": {
          const parsed = parseIntegerArgument(value, argDef, "uint64");
          args[argDef.name] = { type: "uint64", uint64: parsed };
          break;
        }
        case "bytes":
          args[argDef.name] = { type: "bytes", base64: value };
          break;
        case "string":
        default:
          args[argDef.name] = { type: "string", string: value };
          break;
      }
    }

    // Execute function
    const result = await API.executeFunction(
      selectedDevice.provider_id,
      selectedDevice.device_id,
      functionId,
      args,
    );

    // Show success
    feedback.textContent = "Success";
    feedback.className = "function-feedback success";

    console.log(
      `[DeviceDetail] Function ${functionName} executed successfully:`,
      result,
    );

    // Auto-clear success message after 3 seconds
    setTimeout(() => {
      feedback.textContent = "";
      feedback.className = "function-feedback";
    }, 3000);
  } catch (err) {
    console.error(`[DeviceDetail] Function execution failed:`, err);
    feedback.textContent = `Error: ${err.message || "Unknown error"}`;
    feedback.className = "function-feedback error";
  } finally {
    submitBtn.disabled = false;
  }
}

/**
 * Handle state update from SSE (only for selected device)
 */
function handleStateUpdate(data) {
  const key = `${data.provider_id}/${data.device_id}`;

  // Update cache for all devices (for selector and overview)
  if (deviceStates[key]) {
    const signals = deviceStates[key].signals;
    const signalIndex = signals.findIndex(
      (s) => s.signal_id === data.signal_id,
    );

    if (signalIndex >= 0) {
      signals[signalIndex] = {
        signal_id: data.signal_id,
        value: data.value,
        quality: data.quality,
        timestamp_ms: data.timestamp_ms,
      };
    } else {
      signals.push({
        signal_id: data.signal_id,
        value: data.value,
        quality: data.quality,
        timestamp_ms: data.timestamp_ms,
      });
    }
  }

  // Only re-render if this is the selected device
  if (
    selectedDevice &&
    data.provider_id === selectedDevice.provider_id &&
    data.device_id === selectedDevice.device_id
  ) {
    renderDeviceDetail(deviceStates[key]);
  }
}

/**
 * Handle quality change from SSE (only for selected device)
 */
function handleQualityChange(data) {
  const key = `${data.provider_id}/${data.device_id}`;

  // Update cache
  if (deviceStates[key]) {
    const signals = deviceStates[key].signals;
    const signal = signals.find((s) => s.signal_id === data.signal_id);
    if (signal) {
      signal.quality = data.new_quality;
    }
  }

  // Only re-render if this is the selected device
  if (
    selectedDevice &&
    data.provider_id === selectedDevice.provider_id &&
    data.device_id === selectedDevice.device_id
  ) {
    renderDeviceDetail(deviceStates[key]);
  }
}

/**
 * Get device states for other modules (overview)
 */
export function getDeviceStates() {
  return deviceStates;
}

/**
 * Get full device list
 */
export function getDeviceList() {
  return allDevices;
}
