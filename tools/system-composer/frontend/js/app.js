// app.js — Anolis System Composer main entry point
import * as state from './state.js';
import * as sidebar from './sidebar.js';
import * as launch from './launch.js';
import { renderRuntimeForm } from './forms/runtime-form.js';
import { renderProviderList } from './forms/provider-list.js';

let _catalog = null;

async function init() {
  // Session persistence (task 4.14): restore running state if server has one
  let restoredProject = null;
  try {
    const statusRes = await fetch('/api/status');
    const status = await statusRes.json();
    if (status.running && status.active_project) {
      const projRes = await fetch(
        `/api/projects/${encodeURIComponent(status.active_project)}`
      );
      if (projRes.ok) {
        const system = await projRes.json();
        state.setProject(status.active_project, system);
        restoredProject = status.active_project;
      }
    }
  } catch {
    // Non-fatal: proceed without restoring
  }

  // Wire state changes → unsaved indicator + header/action-bar visibility
  state.onStateChange(() => {
    const cur = state.getProject();
    document.getElementById('project-header').classList.toggle('visible', !!cur);
    document.getElementById('action-bar').classList.toggle('visible', !!cur);
    if (cur) {
      document.getElementById('project-name-display').textContent = cur.name;
    }
    document.getElementById('unsaved-indicator').style.display =
      state.isDirty() ? 'inline' : 'none';
  });

  // Load catalog once
  const res = await fetch('/api/catalog');
  _catalog = await res.json();

  // Init sidebar
  await sidebar.init(_onProjectLoaded);

  // Wire save button
  document.getElementById('btn-save').addEventListener('click', _handleSave);

  // If a running project was restored, render its forms and restore running UI
  if (restoredProject) {
    const cur = state.getProject();
    const system = state.getSystem();
    _renderForms(cur.name, system);
    launch.restoreRunningState(cur.name, system);
  }
}

function _onProjectLoaded(name, system) {
  _renderForms(name, system);
  launch.init(name, system);
}

function _renderForms(name, system) {
  const formArea = document.getElementById('form-area');
  formArea.innerHTML = '';
  const onChanged = () => state.markDirty();
  renderRuntimeForm(formArea, system, onChanged);
  renderProviderList(formArea, system, _catalog, onChanged);
}

async function _handleSave() {
  const cur = state.getProject();
  if (!cur) return;

  // Remove any existing error banner
  document.getElementById('error-banner')?.remove();

  const btn = document.getElementById('btn-save');
  btn.disabled = true;
  btn.textContent = 'Saving…';

  try {
    const res = await fetch(`/api/projects/${encodeURIComponent(cur.name)}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(cur.system),
    });
    if (res.ok) {
      state.markClean();
    } else {
      const d = await res.json();
      const banner = document.createElement('div');
      banner.id = 'error-banner';
      banner.className = 'error-banner';
      const detailErrors = Array.isArray(d.errors) ? d.errors : [];
      if (detailErrors.length > 0) {
        banner.innerHTML =
          `<p>${_esc(d.error || 'Save failed')}</p>` +
          '<ul>' +
          detailErrors
            .map((entry) => {
              const path = typeof entry?.path === 'string' ? entry.path : '$';
              const message =
                typeof entry?.message === 'string'
                  ? entry.message
                  : 'Validation error';
              return `<li><code>${_esc(path)}</code>: ${_esc(message)}</li>`;
            })
            .join('') +
          '</ul>';
      } else {
        banner.textContent = d.error || 'Save failed';
      }
      const formArea = document.getElementById('form-area');
      formArea.insertBefore(banner, formArea.firstChild);
    }
  } finally {
    btn.disabled = false;
    btn.textContent = 'Save';
  }
}

function _esc(str) {
  return String(str)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

document.addEventListener('DOMContentLoaded', init);
