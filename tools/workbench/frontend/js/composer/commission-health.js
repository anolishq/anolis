let _projectName = "";
let _pollTimer = null;

function _el(id) {
  return document.getElementById(id);
}

export function start(projectName) {
  _projectName = projectName;
  stop();
  void _refresh();
  _pollTimer = window.setInterval(() => {
    void _refresh();
  }, 2500);
}

export function stop() {
  if (_pollTimer !== null) {
    window.clearInterval(_pollTimer);
    _pollTimer = null;
  }
}

async function _refresh() {
  if (!_projectName) {
    _renderUnavailable("No project selected.");
    return;
  }

  let status;
  try {
    status = await _fetchJson("/api/status");
  } catch (err) {
    _renderUnavailable(`Control API unavailable: ${_message(err)}`);
    return;
  }

  if (!status.running) {
    _renderUnavailable("Runtime is stopped.");
    return;
  }

  const runningProject =
    typeof status.active_project === "string" ? status.active_project : "";
  if (!runningProject) {
    _renderUnavailable("Runtime status is missing active project.");
    return;
  }

  if (runningProject !== _projectName) {
    _renderUnavailable(
      `Runtime is running for \"${runningProject}\". Launch for \"${_projectName}\" is blocked until stop.`,
    );
    return;
  }

  const [runtimeResult, providerResult] = await Promise.allSettled([
    _fetchJson("/v0/runtime/status"),
    _fetchJson("/v0/providers/health"),
  ]);

  if (runtimeResult.status === "fulfilled") {
    _renderRuntimeStatus(runtimeResult.value);
  } else {
    _renderRuntimeStatusUnavailable(runtimeResult.reason);
  }

  if (providerResult.status === "fulfilled") {
    _renderProviderHealth(providerResult.value);
  } else {
    _renderProviderHealthUnavailable(providerResult.reason);
  }
}

function _renderUnavailable(message) {
  const runtimeCard = _el("commission-runtime-card");
  const providerList = _el("commission-provider-health");
  if (!runtimeCard || !providerList) {
    return;
  }

  runtimeCard.innerHTML = `<p class=\"placeholder\">${_esc(message)}</p>`;
  providerList.innerHTML = '<li class="placeholder">Provider health unavailable.</li>';
}

function _renderRuntimeStatus(payload) {
  const runtimeCard = _el("commission-runtime-card");
  if (!runtimeCard) {
    return;
  }

  const mode = typeof payload?.mode === "string" ? payload.mode : "UNKNOWN";
  const uptime = Number(payload?.uptime_seconds);
  const uptimeText = Number.isFinite(uptime) ? `${uptime}s` : "--";
  const pollingInterval = Number(payload?.polling_interval_ms);
  const pollingText = Number.isFinite(pollingInterval) ? `${pollingInterval} ms` : "--";
  const deviceCount = Number(payload?.device_count);
  const deviceCountText = Number.isFinite(deviceCount) ? String(deviceCount) : "--";

  runtimeCard.innerHTML = `
    <div class="commission-runtime-grid">
      <div><span class="meta-label">Mode</span><span class="meta-value">${_esc(mode)}</span></div>
      <div><span class="meta-label">Uptime</span><span class="meta-value">${_esc(uptimeText)}</span></div>
      <div><span class="meta-label">Polling</span><span class="meta-value">${_esc(pollingText)}</span></div>
      <div><span class="meta-label">Devices</span><span class="meta-value">${_esc(deviceCountText)}</span></div>
    </div>
  `;
}

function _renderRuntimeStatusUnavailable(err) {
  const runtimeCard = _el("commission-runtime-card");
  if (!runtimeCard) {
    return;
  }
  runtimeCard.innerHTML = `<p class=\"placeholder\">Runtime status unavailable: ${_esc(_message(err))}</p>`;
}

function _renderProviderHealth(payload) {
  const providerList = _el("commission-provider-health");
  if (!providerList) {
    return;
  }
  providerList.innerHTML = "";

  const providers = Array.isArray(payload?.providers) ? payload.providers : [];
  if (providers.length === 0) {
    providerList.innerHTML = '<li class="placeholder">No provider health data.</li>';
    return;
  }

  for (const provider of providers) {
    const providerId = typeof provider?.provider_id === "string" ? provider.provider_id : "unknown";
    const state = typeof provider?.state === "string" ? provider.state : "UNKNOWN";
    const lifecycle =
      typeof provider?.lifecycle_state === "string" ? provider.lifecycle_state : "--";
    const lastSeenAgo = Number(provider?.last_seen_ago_ms);
    const lastSeenText = Number.isFinite(lastSeenAgo) ? `${lastSeenAgo} ms` : "--";

    const li = document.createElement("li");
    li.className = "commission-provider-row";
    li.innerHTML = `
      <div class="provider-title">
        <span>${_esc(providerId)}</span>
        <span class="badge ${_stateBadgeClass(state)}">${_esc(state)}</span>
      </div>
      <div class="provider-meta">lifecycle: ${_esc(lifecycle)} • last seen: ${_esc(lastSeenText)}</div>
    `;
    providerList.appendChild(li);
  }
}

function _renderProviderHealthUnavailable(err) {
  const providerList = _el("commission-provider-health");
  if (!providerList) {
    return;
  }
  providerList.innerHTML = `<li class="placeholder">Provider health unavailable: ${_esc(_message(err))}</li>`;
}

function _stateBadgeClass(state) {
  const upper = String(state || "").toUpperCase();
  if (upper === "AVAILABLE" || upper === "RUNNING") {
    return "ok";
  }
  if (upper === "UNAVAILABLE" || upper === "STALE") {
    return "unavailable";
  }
  if (upper === "FAULT") {
    return "fault";
  }
  return "unknown";
}

async function _fetchJson(path) {
  const res = await fetch(path);
  const text = await res.text();
  let data = {};
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      if (!res.ok) {
        throw new Error(`HTTP ${res.status}: ${text}`);
      }
      throw new Error(`Invalid JSON response from ${path}`);
    }
  }

  if (!res.ok) {
    const message = data?.status?.message || data?.error || `HTTP ${res.status}`;
    throw new Error(message);
  }

  return data;
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
