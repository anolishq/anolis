import { deriveOperateAvailability, normalizeProviderHealthQuality } from "./operate-state.js";

const INT64_MIN = -9223372036854775808n;
const INT64_MAX = 9223372036854775807n;
const UINT64_MAX = 18446744073709551615n;
const JS_SAFE_MIN = BigInt(Number.MIN_SAFE_INTEGER);
const JS_SAFE_MAX = BigInt(Number.MAX_SAFE_INTEGER);

const state = {
  active: false,
  projectName: null,
  system: null,
  runningProject: null,
  pollTimer: null,
  eventSource: null,
  devices: [],
  selectedKey: "",
  deviceStates: {},
  capabilities: {},
};

let elements = null;
let listenersBound = false;

export function setProjectContext(projectName, system) {
  state.projectName = projectName;
  state.system = system;
  state.devices = [];
  state.selectedKey = "";
  state.deviceStates = {};
  state.capabilities = {};
}

export function activate() {
  if (state.active) {
    return;
  }
  state.active = true;
  _cacheElements();
  _bindListenersOnce();
  void _refreshOperate();
  state.pollTimer = window.setInterval(() => {
    void _refreshOperate();
  }, 5000);
}

export function deactivate() {
  state.active = false;
  if (state.pollTimer !== null) {
    window.clearInterval(state.pollTimer);
    state.pollTimer = null;
  }
  _closeEventSource();
}

async function _refreshOperate() {
  if (!state.active || !state.projectName) {
    return;
  }

  let status;
  try {
    status = await _fetchJson("/api/status");
  } catch (err) {
    _showBanner(`Failed to load runtime status: ${_message(err)}`);
    _clearOperateData();
    return;
  }

  const availability = deriveOperateAvailability(status, state.projectName);
  state.runningProject = availability.runningProject;

  if (!availability.available) {
    _showBanner(availability.message);
    _setModeBadge("--", "unknown");
    _clearOperateData();
    return;
  }

  _hideBanner();
  _ensureEventSource();

  const [modeResult, providerHealthResult, devicesResult] = await Promise.allSettled([
    _fetchJson("/v0/mode"),
    _fetchJson("/v0/providers/health"),
    _fetchJson("/v0/devices"),
  ]);

  if (modeResult.status === "fulfilled") {
    const mode = typeof modeResult.value?.mode === "string" ? modeResult.value.mode : "UNKNOWN";
    _setModeBadge(mode, "ok");
  } else {
    _setModeBadge("UNKNOWN", "unavailable");
  }

  if (providerHealthResult.status === "fulfilled") {
    _renderProviderHealth(providerHealthResult.value);
  } else {
    _renderProviderHealth({ providers: [] });
  }

  if (devicesResult.status === "fulfilled") {
    state.devices = Array.isArray(devicesResult.value?.devices) ? devicesResult.value.devices : [];
    _renderDeviceList();
    await _ensureSelectedDeviceLoaded();
  } else {
    state.devices = [];
    _renderDeviceList();
    _renderNoDeviceSelected("Unable to fetch device list.");
  }
}

function _cacheElements() {
  if (elements) {
    return;
  }
  elements = {
    banner: document.getElementById("operate-banner"),
    modeCurrent: document.getElementById("operate-mode-current"),
    modeSelect: document.getElementById("operate-mode-select"),
    modeSetButton: document.getElementById("operate-set-mode"),
    modeFeedback: document.getElementById("operate-mode-feedback"),
    providerHealthList: document.getElementById("operate-provider-health"),
    deviceList: document.getElementById("operate-device-list"),
    deviceTitle: document.getElementById("operate-device-title"),
    deviceDetail: document.getElementById("operate-device-detail"),
  };
}

function _bindListenersOnce() {
  if (listenersBound || !elements) {
    return;
  }
  listenersBound = true;

  elements.modeSetButton.addEventListener("click", async () => {
    if (!state.projectName || state.runningProject !== state.projectName) {
      return;
    }
    const mode = elements.modeSelect.value;
    elements.modeSetButton.disabled = true;
    elements.modeFeedback.textContent = "Setting mode...";
    try {
      await _fetchJson("/v0/mode", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mode }),
      });
      elements.modeFeedback.textContent = `Mode set to ${mode}`;
      elements.modeFeedback.className = "field-note";
      void _refreshOperate();
    } catch (err) {
      elements.modeFeedback.textContent = `Failed to set mode: ${_message(err)}`;
      elements.modeFeedback.className = "field-error";
    } finally {
      elements.modeSetButton.disabled = false;
    }
  });
}

function _showBanner(message) {
  if (!elements) {
    return;
  }
  elements.banner.textContent = message;
  elements.banner.classList.remove("hidden");
}

function _hideBanner() {
  if (!elements) {
    return;
  }
  elements.banner.classList.add("hidden");
  elements.banner.textContent = "";
}

function _setModeBadge(text, className) {
  if (!elements) {
    return;
  }
  elements.modeCurrent.textContent = text;
  elements.modeCurrent.className = `badge ${className}`;
}

function _renderProviderHealth(payload) {
  if (!elements) {
    return;
  }

  const list = elements.providerHealthList;
  list.innerHTML = "";

  const providers = Array.isArray(payload?.providers) ? payload.providers : [];
  if (providers.length === 0) {
    list.innerHTML = '<li class="placeholder">No provider health available.</li>';
    return;
  }

  for (const entry of providers) {
    const providerId = typeof entry?.provider_id === "string" ? entry.provider_id : "unknown";
    const quality = normalizeProviderHealthQuality(entry?.health?.quality || entry?.quality || "UNKNOWN");
    const li = document.createElement("li");
    li.innerHTML = `${_esc(providerId)} <span class="badge ${quality.toLowerCase()}">${_esc(quality)}</span>`;
    list.appendChild(li);
  }
}

function _renderDeviceList() {
  if (!elements) {
    return;
  }

  const list = elements.deviceList;
  list.innerHTML = "";

  if (!Array.isArray(state.devices) || state.devices.length === 0) {
    list.innerHTML = '<li class="placeholder">No devices found.</li>';
    return;
  }

  for (const device of state.devices) {
    const key = `${device.provider_id}/${device.device_id}`;
    const li = document.createElement("li");
    const button = document.createElement("button");
    const title = device.display_name || device.device_id || key;
    button.textContent = `${title} (${key})`;
    button.addEventListener("click", () => {
      state.selectedKey = key;
      void _ensureSelectedDeviceLoaded();
    });
    li.appendChild(button);
    if (key === state.selectedKey) {
      li.style.borderColor = "#1d4ed8";
    }
    list.appendChild(li);
  }
}

async function _ensureSelectedDeviceLoaded() {
  if (state.devices.length === 0) {
    _renderNoDeviceSelected();
    return;
  }

  if (!state.selectedKey) {
    const first = state.devices[0];
    state.selectedKey = `${first.provider_id}/${first.device_id}`;
    _renderDeviceList();
  }

  const selected = _getSelectedDevice();
  if (!selected) {
    _renderNoDeviceSelected();
    return;
  }

  const key = state.selectedKey;

  try {
    const valuesPayload = await _fetchJson(`/v0/state/${encodeURIComponent(selected.provider_id)}/${encodeURIComponent(selected.device_id)}`);
    state.deviceStates[key] = Array.isArray(valuesPayload?.values)
      ? valuesPayload.values.map((entry) => _normalizeStateSignal(entry))
      : [];
  } catch {
    state.deviceStates[key] = [];
  }

  if (!state.capabilities[key]) {
    try {
      const capsPayload = await _fetchJson(
        `/v0/devices/${encodeURIComponent(selected.provider_id)}/${encodeURIComponent(selected.device_id)}/capabilities`,
      );
      const normalizedCaps = _normalizeCapabilities(capsPayload?.capabilities || {});
      state.capabilities[key] = normalizedCaps;
    } catch {
      state.capabilities[key] = { functions: [] };
    }
  }

  _renderSelectedDevice();
}

function _renderSelectedDevice() {
  if (!elements) {
    return;
  }

  const selected = _getSelectedDevice();
  if (!selected) {
    _renderNoDeviceSelected();
    return;
  }

  const key = state.selectedKey;
  const values = Array.isArray(state.deviceStates[key]) ? state.deviceStates[key] : [];
  const caps = state.capabilities[key] || { functions: [] };

  elements.deviceTitle.textContent = `${selected.display_name || selected.device_id} (${key})`;

  let html = "";
  if (values.length === 0) {
    html += '<p class="placeholder">No signals available.</p>';
  } else {
    html += '<table class="signal-table"><thead><tr><th>Signal</th><th>Value</th><th>Quality</th><th>Timestamp</th></tr></thead><tbody>';
    for (const signal of values) {
      const quality = _normalizeQuality(signal.quality);
      const timestamp = signal.timestamp_ms > 0 ? new Date(signal.timestamp_ms).toLocaleTimeString() : "--";
      html += `<tr><td>${_esc(signal.signal_id || "-")}</td><td>${_esc(_formatValue(signal.value))}</td><td>${_esc(quality)}</td><td>${_esc(timestamp)}</td></tr>`;
    }
    html += "</tbody></table>";
  }

  const functions = Array.isArray(caps.functions) ? caps.functions : [];
  if (functions.length === 0) {
    html += '<p class="placeholder">No callable functions declared.</p>';
  } else {
    for (const func of functions) {
      html += _renderFunctionForm(key, func);
    }
  }

  elements.deviceDetail.innerHTML = html;
  for (const form of elements.deviceDetail.querySelectorAll(".function-form")) {
    form.addEventListener("submit", _handleFunctionSubmit);
  }
}

function _renderNoDeviceSelected(message = "Select a device to inspect signals and execute functions.") {
  if (!elements) {
    return;
  }
  elements.deviceTitle.textContent = "Device Detail";
  elements.deviceDetail.innerHTML = `<p class=\"placeholder\">${_esc(message)}</p>`;
}

function _renderFunctionForm(deviceKey, func) {
  const formId = `operate-fn-${deviceKey.replaceAll("/", "-")}-${func.function_id}`;
  const functionName = func.display_name || func.name || func.function_name || `Function ${func.function_id}`;
  const description = func.description || func.label || "";

  let html = `<div class=\"function-card\"><form class=\"function-form\" id=\"${_esc(formId)}\" data-device-key=\"${_esc(deviceKey)}\" data-function-id=\"${func.function_id}\"><h4>${_esc(functionName)}</h4>`;
  if (description) {
    html += `<p class=\"function-description\">${_esc(description)}</p>`;
  }

  const args = Array.isArray(func.args) ? func.args : [];
  for (const [argIndex, arg] of args.entries()) {
    html += _renderArgInput(arg, argIndex);
  }

  html += `<div class=\"function-actions\"><button type=\"submit\" class=\"btn-secondary btn-sm\">Execute</button><span class=\"function-feedback\"></span></div></form></div>`;
  return html;
}

function _renderArgInput(arg, argIndex) {
  const inputName = `arg_${argIndex}`;
  const argId = `arg-${argIndex}-${Math.random().toString(16).slice(2, 8)}`;
  const required = arg.required !== false;
  const requiredLabel = required ? ' <span class="required">*</span>' : "";

  let input = "";
  const type = arg.type || "string";
  if (type === "double") {
    input = `<input type=\"number\" id=\"${argId}\" name=\"${inputName}\" step=\"any\" ${required ? "required" : ""}>`;
  } else if (type === "int64" || type === "uint64") {
    input = `<input type=\"text\" id=\"${argId}\" name=\"${inputName}\" inputmode=\"numeric\" ${required ? "required" : ""}>`;
  } else if (type === "bool") {
    input = `<input type=\"checkbox\" id=\"${argId}\" name=\"${inputName}\">`;
  } else if (type === "bytes") {
    input = `<input type=\"text\" id=\"${argId}\" name=\"${inputName}\" placeholder=\"Base64 encoded\" ${required ? "required" : ""}>`;
  } else {
    input = `<input type=\"text\" id=\"${argId}\" name=\"${inputName}\" ${required ? "required" : ""}>`;
  }

  let hint = "";
  if (arg.min !== undefined && arg.max !== undefined) {
    hint = ` <span class=\"constraint-hint\">[${_esc(String(arg.min))} - ${_esc(String(arg.max))}]</span>`;
  }

  return `<div class=\"arg-row\"><label for=\"${argId}\">${_esc(arg.name)}${requiredLabel}${hint}</label>${input}</div>`;
}

async function _handleFunctionSubmit(event) {
  event.preventDefault();

  const form = event.target;
  const deviceKey = form.dataset.deviceKey || "";
  const functionId = Number.parseInt(form.dataset.functionId || "", 10);
  const feedback = form.querySelector(".function-feedback");
  const button = form.querySelector('button[type="submit"]');

  if (!deviceKey || Number.isNaN(functionId) || !feedback || !button) {
    return;
  }

  const [providerId, deviceId] = deviceKey.split("/");
  const func = (state.capabilities[deviceKey]?.functions || []).find((entry) => entry.function_id === functionId);
  if (!providerId || !deviceId || !func) {
    feedback.textContent = "Function definition not found.";
    feedback.className = "function-feedback error";
    return;
  }

  button.disabled = true;
  feedback.textContent = "Executing...";
  feedback.className = "function-feedback";

  try {
    const argsPayload = _buildArgsPayload(form, func);
    await _fetchJson("/v0/call", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        provider_id: providerId,
        device_id: deviceId,
        function_id: functionId,
        args: argsPayload,
      }),
    });

    feedback.textContent = "Success";
    feedback.className = "function-feedback success";
    window.setTimeout(() => {
      feedback.textContent = "";
      feedback.className = "function-feedback";
    }, 2500);
  } catch (err) {
    feedback.textContent = `Error: ${_message(err)}`;
    feedback.className = "function-feedback error";
  } finally {
    button.disabled = false;
  }
}

function _buildArgsPayload(form, func) {
  const args = {};
  const formData = new FormData(form);

  for (const [argIndex, argDef] of (func.args || []).entries()) {
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
      args[argDef.name] = { type: "bool", bool: Boolean(inputElement.checked) };
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

    if (argType === "double") {
      const parsed = Number(value);
      if (Number.isNaN(parsed)) {
        throw new Error(`Invalid double argument: ${argDef.name}`);
      }
      args[argDef.name] = { type: "double", double: parsed };
      continue;
    }

    if (argType === "int64") {
      const parsed = _parseIntegerArgument(value, argDef, "int64");
      args[argDef.name] = { type: "int64", int64: parsed };
      continue;
    }

    if (argType === "uint64") {
      const parsed = _parseIntegerArgument(value, argDef, "uint64");
      args[argDef.name] = { type: "uint64", uint64: parsed };
      continue;
    }

    if (argType === "bytes") {
      args[argDef.name] = { type: "bytes", base64: value };
      continue;
    }

    args[argDef.name] = { type: "string", string: value };
  }

  return args;
}

function _parseIntegerArgument(value, argDef, valueType) {
  const argName = argDef.name;
  let parsedBigInt;
  try {
    parsedBigInt = BigInt(value);
  } catch {
    throw new Error(`Invalid ${valueType} argument: ${argName}`);
  }

  if (valueType === "int64") {
    if (parsedBigInt < INT64_MIN || parsedBigInt > INT64_MAX) {
      throw new Error(`Out-of-range int64 argument: ${argName}`);
    }
    if (parsedBigInt < JS_SAFE_MIN || parsedBigInt > JS_SAFE_MAX) {
      throw new Error(`int64 argument out of browser-safe range: ${argName}`);
    }
  } else {
    if (parsedBigInt < 0n || parsedBigInt > UINT64_MAX) {
      throw new Error(`Out-of-range uint64 argument: ${argName}`);
    }
    if (parsedBigInt > JS_SAFE_MAX) {
      throw new Error(`uint64 argument out of browser-safe range: ${argName}`);
    }
  }

  return Number(parsedBigInt);
}

function _getSelectedDevice() {
  if (!state.selectedKey) {
    return null;
  }
  return state.devices.find((entry) => `${entry.provider_id}/${entry.device_id}` === state.selectedKey) || null;
}

function _clearOperateData() {
  state.devices = [];
  state.selectedKey = "";
  state.deviceStates = {};
  state.capabilities = {};
  _renderProviderHealth({ providers: [] });
  _renderDeviceList();
  _renderNoDeviceSelected("Runtime data unavailable.");
  _closeEventSource();
}

function _ensureEventSource() {
  if (state.eventSource || !state.active) {
    return;
  }

  const source = new EventSource("/v0/events");
  source.addEventListener("state_update", (event) => {
    _consumeStateEvent(event.data);
  });
  source.addEventListener("quality_change", (event) => {
    _consumeQualityEvent(event.data);
  });
  source.onerror = () => {
    // Let EventSource retry; no-op.
  };
  state.eventSource = source;
}

function _closeEventSource() {
  if (!state.eventSource) {
    return;
  }
  state.eventSource.close();
  state.eventSource = null;
}

function _consumeStateEvent(rawData) {
  let payload;
  try {
    payload = JSON.parse(rawData);
  } catch {
    return;
  }

  const key = `${payload.provider_id}/${payload.device_id}`;
  if (!state.deviceStates[key]) {
    state.deviceStates[key] = [];
  }
  const values = state.deviceStates[key];
  const idx = values.findIndex((entry) => entry.signal_id === payload.signal_id);
  const normalized = _normalizeStateSignal(payload);
  if (idx >= 0) {
    values[idx] = normalized;
  } else {
    values.push(normalized);
  }

  if (state.selectedKey === key && !_isFunctionInputFocused()) {
    _renderSelectedDevice();
  }
}

function _consumeQualityEvent(rawData) {
  let payload;
  try {
    payload = JSON.parse(rawData);
  } catch {
    return;
  }

  const key = `${payload.provider_id}/${payload.device_id}`;
  if (!state.deviceStates[key]) {
    return;
  }
  const signal = state.deviceStates[key].find((entry) => entry.signal_id === payload.signal_id);
  if (signal) {
    signal.quality = payload.new_quality;
  }

  if (state.selectedKey === key && !_isFunctionInputFocused()) {
    _renderSelectedDevice();
  }
}

function _isFunctionInputFocused() {
  const active = document.activeElement;
  if (!(active instanceof HTMLElement)) {
    return false;
  }
  return active.closest(".function-form") !== null;
}

async function _fetchJson(path, options = {}) {
  const response = await fetch(path, options);
  const text = await response.text();
  let data = {};
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}: ${text}`);
      }
      return {};
    }
  }

  if (!response.ok) {
    const message = data?.status?.message || data?.error || `HTTP ${response.status}`;
    throw new Error(message);
  }

  return data;
}

function _normalizeCapabilities(capabilities) {
  return {
    ...capabilities,
    functions: _normalizeFunctions(capabilities?.functions),
  };
}

function _normalizeFunctions(functions) {
  if (!Array.isArray(functions)) {
    return [];
  }

  const normalized = functions.map((func, index) => {
    const functionId = Number(func?.function_id);
    const fallbackId = Number.isFinite(functionId) ? functionId : index + 1;

    const name =
      typeof func?.name === "string" && func.name.trim() !== ""
        ? func.name.trim()
        : typeof func?.function_name === "string" && func.function_name.trim() !== ""
          ? func.function_name.trim()
          : `Function ${fallbackId}`;

    const description =
      typeof func?.label === "string" && func.label.trim() !== ""
        ? func.label.trim()
        : typeof func?.description === "string" && func.description.trim() !== ""
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
      args: _normalizeFunctionArgs(func?.args),
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

function _normalizeFunctionArgs(args) {
  if (Array.isArray(args)) {
    return args
      .map((arg, index) => _normalizeArgSpec(arg, `arg_${index + 1}`))
      .filter((arg) => arg.name !== "");
  }

  if (args && typeof args === "object") {
    return Object.entries(args)
      .map(([name, spec]) => _normalizeArgSpec(spec, name))
      .filter((arg) => arg.name !== "")
      .sort((a, b) => a.name.localeCompare(b.name));
  }

  return [];
}

function _normalizeArgSpec(arg, fallbackName = "") {
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
  };
}

function _normalizeStateSignal(signal) {
  const timestampMs = Number(signal?.timestamp_ms ?? signal?.timestamp_epoch_ms ?? 0);
  return {
    signal_id: signal?.signal_id,
    value: signal?.value,
    quality: signal?.quality,
    timestamp_ms: Number.isFinite(timestampMs) ? timestampMs : 0,
  };
}

function _normalizeQuality(quality) {
  return normalizeProviderHealthQuality(quality);
}

function _formatValue(value) {
  if (typeof value === "object" && value !== null) {
    return JSON.stringify(value);
  }
  return String(value);
}

function _message(err) {
  if (err instanceof Error) {
    return err.message;
  }
  return String(err);
}

function _esc(str) {
  return String(str)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/\"/g, "&quot;")
    .replace(/'/g, "&#39;");
}
