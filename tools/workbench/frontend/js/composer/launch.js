// launch.js — Preflight, launch, stop, restart, health badge, operator-ui link
import { connect as logConnect, disconnect as logDisconnect } from './log-pane.js';
import { startPolling, stopPolling } from './health.js';
import { deriveLaunchBlockReason } from './launch-guards.js';

let _name = null;
let _system = null;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/** Initialise the launch panel in idle state for a newly-loaded project. */
export function init(projectName, system) {
  _name = projectName;
  _system = system;
  logDisconnect();
  stopPolling();
  document.getElementById('log-pane').classList.remove('visible');
  const panel = document.getElementById('launch-panel');
  _renderIdle(panel);
  panel.classList.add('visible');
}

/** Restore running state when the tab is reopened while a system is live. */
export function restoreRunningState(projectName, system) {
  _name = projectName;
  _system = system;
  const panel = document.getElementById('launch-panel');
  _renderRunning(panel);
  panel.classList.add('visible');
  logConnect(projectName);
  startPolling(system, _onHealthChange);
}

// ---------------------------------------------------------------------------
// Render helpers
// ---------------------------------------------------------------------------

function _renderIdle(panel) {
  panel.innerHTML = `
    <div class="launch-idle-bar">
      <button class="btn-secondary" id="btn-preflight">Preflight Check</button>
      <button class="btn-primary" id="btn-launch-run" disabled>Launch →</button>
      <span id="preflight-summary" class="launch-summary"></span>
    </div>
    <div id="preflight-results" class="preflight-results"></div>
  `;
  document.getElementById('btn-preflight').addEventListener('click', _runPreflight);
  document.getElementById('btn-launch-run').addEventListener('click', _doLaunch);
}

function _renderRunning(panel) {
  const port = _system?.topology?.runtime?.http_port ?? '?';
  panel.innerHTML = `
    <div class="launch-running-bar">
      <span class="running-label">Running — ${_esc(String(_name))} on port ${_esc(String(port))}</span>
      <span id="health-badge" class="health-badge starting">Starting…</span>
      <button class="btn-secondary btn-sm" id="btn-stop">Stop</button>
      <button class="btn-secondary btn-sm" id="btn-restart"
        title="Restarts with last saved config. Unsaved edits will not be applied.">
        Restart
      </button>
    </div>
    <div id="operator-ui-link" style="display:none;margin-top:8px;font-size:12px;">
      <a href="#" id="operator-ui-anchor" target="_blank" rel="noopener noreferrer">
        Open in Operator UI →
      </a>
      <span class="launch-summary" style="margin-left:8px;">
        Operator UI base URL is configurable.
      </span>
    </div>
  `;
  document.getElementById('btn-stop').addEventListener('click', _doStop);
  document.getElementById('btn-restart').addEventListener('click', _doRestart);
}

// ---------------------------------------------------------------------------
// Preflight
// ---------------------------------------------------------------------------

async function _runPreflight() {
  const btn = document.getElementById('btn-preflight');
  btn.disabled = true;
  btn.textContent = 'Checking…';
  document.getElementById('preflight-results').innerHTML = '';
  const summary = document.getElementById('preflight-summary');
  summary.textContent = '';
  summary.style.color = '';

  try {
    const res = await fetch(
      `/api/projects/${encodeURIComponent(_name)}/preflight`,
      { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' }
    );
    const data = await res.json();
    _renderPreflightResults(data);
    const launchBtn = document.getElementById('btn-launch-run');
    if (launchBtn) launchBtn.disabled = !data.ok;
  } catch (err) {
    if (summary) {
      summary.textContent = `Error: ${err.message}`;
      summary.style.color = 'var(--danger)';
    }
  } finally {
    btn.disabled = false;
    btn.textContent = 'Preflight Check';
  }
}

function _renderPreflightResults(data) {
  const container = document.getElementById('preflight-results');
  if (!container) return;

  const summary = document.getElementById('preflight-summary');
  if (summary) {
    summary.textContent = data.ok ? '✓ All checks passed' : '✗ Checks failed';
    summary.style.color = data.ok ? 'var(--success)' : 'var(--danger)';
  }

  const items = (data.checks || []).map(c => {
    let icon, cls;
    if (c.ok === true)       { icon = '✓'; cls = 'check-ok'; }
    else if (c.ok === false) { icon = '✗'; cls = 'check-fail'; }
    else                     { icon = '–'; cls = 'check-skip'; }

    let detail = '';
    if (c.error) detail += `<span class="check-detail">${_esc(c.error)}</span> `;
    if (c.hint)  detail += `<span class="check-hint">${_esc(c.hint)}</span>`;
    if (c.note)  detail += `<span class="check-detail">${_esc(c.note)}</span>`;

    return `<div class="check-item ${cls}">
      <span class="check-icon">${icon}</span>
      <span class="check-name">${_esc(c.name)}</span>
      ${detail ? `<span class="check-extra">${detail}</span>` : ''}
    </div>`;
  });
  container.innerHTML = items.join('');
}

// ---------------------------------------------------------------------------
// Launch
// ---------------------------------------------------------------------------

async function _doLaunch() {
  const btn = document.getElementById('btn-launch-run');
  btn.disabled = true;
  btn.textContent = 'Launching…';

  try {
    const statusRes = await fetch('/api/status');
    if (statusRes.ok) {
      const status = await statusRes.json().catch(() => ({}));
      const blockReason = deriveLaunchBlockReason(status, _name);
      if (blockReason) {
        const summary = document.getElementById('preflight-summary');
        if (summary) {
          summary.textContent = blockReason;
          summary.style.color = 'var(--danger)';
        }
        btn.disabled = false;
        btn.textContent = 'Launch →';
        return;
      }
    }

    const res = await fetch(
      `/api/projects/${encodeURIComponent(_name)}/launch`,
      { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' }
    );
    if (res.ok) {
      const panel = document.getElementById('launch-panel');
      _renderRunning(panel);
      logConnect(_name);
      startPolling(_system, _onHealthChange);
    } else {
      const d = await res.json().catch(() => ({}));
      const summary = document.getElementById('preflight-summary');
      if (summary) {
        const conflictMessage =
          typeof d?.error === 'string' && d.error.toLowerCase().includes('already running')
            ? 'Launch blocked: runtime is already active. Stop current runtime before launching this project.'
            : null;
        summary.textContent = conflictMessage || `Launch failed: ${d.error || 'unknown error'}`;
        summary.style.color = 'var(--danger)';
      }
      btn.disabled = false;
      btn.textContent = 'Launch →';
    }
  } catch (err) {
    const summary = document.getElementById('preflight-summary');
    if (summary) {
      summary.textContent = `Launch error: ${err.message}`;
      summary.style.color = 'var(--danger)';
    }
    btn.disabled = false;
    btn.textContent = 'Launch →';
  }
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

async function _doStop() {
  const btn = document.getElementById('btn-stop');
  btn.disabled = true;
  btn.textContent = 'Stopping…';
  try {
    await fetch(
      `/api/projects/${encodeURIComponent(_name)}/stop`,
      { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' }
    );
  } finally {
    stopPolling();
    logDisconnect();
    const panel = document.getElementById('launch-panel');
    _renderIdle(panel);
  }
}

// ---------------------------------------------------------------------------
// Restart
// ---------------------------------------------------------------------------

async function _doRestart() {
  const btn = document.getElementById('btn-restart');
  btn.disabled = true;
  btn.textContent = 'Restarting…';
  try {
    const res = await fetch(
      `/api/projects/${encodeURIComponent(_name)}/restart`,
      { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' }
    );
    if (res.ok) {
      stopPolling();
      logDisconnect();
      const panel = document.getElementById('launch-panel');
      _renderRunning(panel);
      logConnect(_name);
      startPolling(_system, _onHealthChange);
    } else {
      const d = await res.json().catch(() => ({}));
      const message = d.error || 'Restart failed';
      alert(message);
      btn.disabled = false;
      btn.textContent = 'Restart';
    }
  } catch {
    alert('Restart failed');
    btn.disabled = false;
    btn.textContent = 'Restart';
  }
}

// ---------------------------------------------------------------------------
// Health badge callback
// ---------------------------------------------------------------------------

function _onHealthChange(status) {
  const badge = document.getElementById('health-badge');
  if (!badge) return;
  badge.className = `health-badge ${status}`;
  const labels = { healthy: 'Healthy', unavailable: 'Unavailable', starting: 'Starting…' };
  badge.textContent = labels[status] ?? status;

  // Show/hide operator-ui link
  const linkDiv = document.getElementById('operator-ui-link');
  if (!linkDiv) return;
  if (status === 'healthy') {
    const bind = _system?.topology?.runtime?.http_bind || '127.0.0.1';
    const port = _system?.topology?.runtime?.http_port || 8080;
    const runtimeApi = `http://${bind}:${port}`;
    const operatorUiBase = resolveOperatorUiBase(_system);
    const link = `${operatorUiBase}?api=${encodeURIComponent(runtimeApi)}`;
    const anchor = document.getElementById('operator-ui-anchor');
    if (anchor) anchor.href = link;
    linkDiv.style.display = 'block';
  } else {
    linkDiv.style.display = 'none';
  }
}

export function resolveOperatorUiBase(system) {
  const explicit = system?.topology?.runtime?.operator_ui_base;
  if (typeof explicit === 'string' && explicit.trim() !== '') {
    return trimTrailingSlash(explicit.trim());
  }

  const corsOrigins = system?.topology?.runtime?.cors_origins;
  if (Array.isArray(corsOrigins)) {
    const fromCors = corsOrigins.find(
      origin => typeof origin === 'string' && /^https?:\/\//i.test(origin)
    );
    if (typeof fromCors === 'string' && fromCors.trim() !== '') {
      return trimTrailingSlash(fromCors.trim());
    }
  }

  const fromComposerStatus = window.__ANOLIS_COMPOSER__?.operatorUiBase;
  if (typeof fromComposerStatus === 'string' && fromComposerStatus.trim() !== '') {
    return trimTrailingSlash(fromComposerStatus.trim());
  }

  return 'http://localhost:3000';
}

export function trimTrailingSlash(url) {
  return url.endsWith('/') ? url.slice(0, -1) : url;
}

// ---------------------------------------------------------------------------
// Escape helper
// ---------------------------------------------------------------------------

function _esc(str) {
  return String(str)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}
