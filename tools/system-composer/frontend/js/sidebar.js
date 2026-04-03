// sidebar.js — project list, modals, and project loading

import * as state from './state.js';

const API = '/api';
let _onProjectLoaded = null;

export async function init(onProjectLoaded) {
  _onProjectLoaded = onProjectLoaded;
  document.getElementById('btn-new-project').addEventListener('click', showNewProjectModal);
  await refresh();
}

export async function refresh() {
  const res = await fetch(`${API}/projects`);
  const projects = await res.json();
  _renderList(projects);
}

function _renderList(projects) {
  const ul = document.getElementById('project-list');
  ul.innerHTML = '';
  const cur = state.getProject();
  for (const p of projects) {
    const li = document.createElement('li');
    li.className = 'project-entry' + (cur?.name === p.name ? ' active' : '');

    const nameBtn = document.createElement('button');
    nameBtn.className = 'project-name-btn';
    nameBtn.textContent = p.name;
    nameBtn.addEventListener('click', () => _handleClick(p.name));

    const menuBtn = document.createElement('button');
    menuBtn.className = 'project-menu-btn';
    menuBtn.setAttribute('aria-label', 'More options');
    menuBtn.textContent = '⋯';
    menuBtn.addEventListener('click', e => { e.stopPropagation(); _showCtxMenu(p.name, menuBtn); });

    li.append(nameBtn, menuBtn);
    ul.appendChild(li);
  }
}

async function _handleClick(name) {
  if (state.isDirty() && !confirm(`You have unsaved changes. Switch to "${name}" anyway?`)) return;
  await _loadProject(name);
}

async function _loadProject(name) {
  const res = await fetch(`${API}/projects/${encodeURIComponent(name)}`);
  if (!res.ok) { alert(`Failed to load "${name}"`); return; }
  const system = await res.json();
  state.setProject(name, system);
  await refresh();
  _onProjectLoaded?.(name, system);
}

// ---- Context menu ----

function _showCtxMenu(name, anchor) {
  document.querySelectorAll('.ctx-menu').forEach(el => el.remove());
  const menu = document.createElement('div');
  menu.className = 'ctx-menu';

  for (const { label, fn, danger } of [
    { label: 'Rename…',    fn: () => _showRenameModal(name) },
    { label: 'Duplicate…', fn: () => _showDuplicateModal(name) },
    { label: 'Delete…',    fn: () => _confirmDelete(name), danger: true },
  ]) {
    const btn = document.createElement('button');
    btn.textContent = label;
    if (danger) btn.classList.add('danger');
    btn.addEventListener('click', () => { menu.remove(); fn(); });
    menu.appendChild(btn);
  }

  document.body.appendChild(menu);
  const r = anchor.getBoundingClientRect();
  menu.style.top  = `${r.bottom + window.scrollY + 4}px`;
  menu.style.left = `${r.left  + window.scrollX}px`;

  const dismiss = e => {
    if (!menu.contains(e.target)) { menu.remove(); document.removeEventListener('mousedown', dismiss); }
  };
  setTimeout(() => document.addEventListener('mousedown', dismiss), 0);
}

// ---- Modals ----

export function showModal(html) {
  const overlay = document.getElementById('modal-overlay');
  overlay.removeAttribute('aria-hidden');
  document.getElementById('modal-container').innerHTML = html;
  overlay.style.display = 'flex';
}

export function closeModal() {
  const overlay = document.getElementById('modal-overlay');
  overlay.style.display = 'none';
  overlay.setAttribute('aria-hidden', 'true');
  document.getElementById('modal-container').innerHTML = '';
}

async function showNewProjectModal() {
  const res = await fetch(`${API}/templates`);
  const templates = await res.json();

  showModal(`
    <h2>New Project</h2>
    <div class="form-group">
      <label for="modal-name">Project name</label>
      <input id="modal-name" type="text" placeholder="my-system" maxlength="64" autocomplete="off" spellcheck="false">
      <span class="field-error" id="modal-name-err" style="display:none"></span>
    </div>
    <div class="form-group">
      <label>Template</label>
      <div class="template-list">
        ${templates.map(t => `
          <label class="template-option">
            <input type="radio" name="tpl" value="${_esc(t.id)}">
            <div>
              <strong>${_esc(t.meta.name || t.id)}</strong>
              <span>${_esc(t.meta.description || '')}</span>
            </div>
          </label>`).join('')}
      </div>
      <span class="field-error" id="modal-tpl-err" style="display:none"></span>
    </div>
    <div class="modal-actions">
      <button id="modal-cancel">Cancel</button>
      <button id="modal-ok" class="btn-primary">Create</button>
    </div>
  `);

  const firstRadio = document.querySelector('input[name="tpl"]');
  if (firstRadio) firstRadio.checked = true;
  document.getElementById('modal-name').focus();

  document.getElementById('modal-cancel').addEventListener('click', closeModal);
  document.getElementById('modal-ok').addEventListener('click', async () => {
    const name    = document.getElementById('modal-name').value.trim();
    const nameErr = document.getElementById('modal-name-err');
    const tplErr  = document.getElementById('modal-tpl-err');
    nameErr.style.display = 'none';
    tplErr.style.display  = 'none';

    if (!_validName(name)) {
      nameErr.textContent  = 'Name must be 1–64 characters: letters, digits, hyphens, underscores.';
      nameErr.style.display = 'block'; return;
    }
    const tplRadio = document.querySelector('input[name="tpl"]:checked');
    if (!tplRadio) {
      tplErr.textContent  = 'Please select a template.';
      tplErr.style.display = 'block'; return;
    }

    const res = await fetch(`${API}/projects`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name, template: tplRadio.value }),
    });
    if (!res.ok) {
      const d = await res.json();
      nameErr.textContent  = d.error || 'Failed to create project';
      nameErr.style.display = 'block'; return;
    }
    const system = await res.json();
    closeModal();
    state.setProject(name, system);
    await refresh();
    _onProjectLoaded?.(name, system);
  });
}

async function _showRenameModal(name) {
  showModal(`
    <h2>Rename Project</h2>
    <div class="form-group">
      <label for="modal-name">New name</label>
      <input id="modal-name" type="text" value="${_esc(name)}" maxlength="64" autocomplete="off" spellcheck="false">
      <span class="field-error" id="modal-name-err" style="display:none"></span>
    </div>
    <div class="modal-actions">
      <button id="modal-cancel">Cancel</button>
      <button id="modal-ok" class="btn-primary">Rename</button>
    </div>
  `);
  const inp = document.getElementById('modal-name');
  inp.select();
  document.getElementById('modal-cancel').addEventListener('click', closeModal);
  document.getElementById('modal-ok').addEventListener('click', async () => {
    const newName = inp.value.trim();
    const err = document.getElementById('modal-name-err');
    err.style.display = 'none';
    if (!_validName(newName)) {
      err.textContent  = 'Name must be 1–64 characters: letters, digits, hyphens, underscores.';
      err.style.display = 'block'; return;
    }
    const res = await fetch(`${API}/projects/${encodeURIComponent(name)}/rename`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ new_name: newName }),
    });
    if (!res.ok) {
      const d = await res.json();
      err.textContent  = d.error || 'Rename failed';
      err.style.display = 'block'; return;
    }
    closeModal();
    const cur = state.getProject();
    if (cur?.name === name) {
      state.setProject(newName, cur.system);
      _onProjectLoaded?.(newName, cur.system);
    }
    await refresh();
  });
}

async function _showDuplicateModal(name) {
  showModal(`
    <h2>Duplicate Project</h2>
    <div class="form-group">
      <label for="modal-name">New project name</label>
      <input id="modal-name" type="text" placeholder="${_esc(name)}-copy" maxlength="64" autocomplete="off" spellcheck="false">
      <span class="field-error" id="modal-name-err" style="display:none"></span>
    </div>
    <div class="modal-actions">
      <button id="modal-cancel">Cancel</button>
      <button id="modal-ok" class="btn-primary">Duplicate</button>
    </div>
  `);
  document.getElementById('modal-name').focus();
  document.getElementById('modal-cancel').addEventListener('click', closeModal);
  document.getElementById('modal-ok').addEventListener('click', async () => {
    const newName = document.getElementById('modal-name').value.trim();
    const err = document.getElementById('modal-name-err');
    err.style.display = 'none';
    if (!_validName(newName)) {
      err.textContent  = 'Name must be 1–64 characters: letters, digits, hyphens, underscores.';
      err.style.display = 'block'; return;
    }
    const res = await fetch(`${API}/projects/${encodeURIComponent(name)}/duplicate`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ new_name: newName }),
    });
    if (!res.ok) {
      const d = await res.json();
      err.textContent  = d.error || 'Duplicate failed';
      err.style.display = 'block'; return;
    }
    closeModal();
    await refresh();
  });
}

async function _confirmDelete(name) {
  showModal(`
    <h2>Delete Project</h2>
    <p>Delete <strong>${_esc(name)}</strong>? This cannot be undone.</p>
    <div class="modal-actions">
      <button id="modal-cancel">Cancel</button>
      <button id="modal-ok" class="btn-danger">Delete</button>
    </div>
  `);
  document.getElementById('modal-cancel').addEventListener('click', closeModal);
  document.getElementById('modal-ok').addEventListener('click', async () => {
    const res = await fetch(`${API}/projects/${encodeURIComponent(name)}`, { method: 'DELETE' });
    if (!res.ok) {
      const d = await res.json();
      alert(d.error || 'Delete failed');
      closeModal(); return;
    }
    closeModal();
    if (state.getProject()?.name === name) {
      state.setProject(null, null);
      const fa = document.getElementById('form-area');
      fa.innerHTML = '<p id="empty-state">Select a project or create a new one to get started.</p>';
    }
    await refresh();
  });
}

// ---- Helpers ----

function _validName(name) {
  return /^[a-zA-Z0-9_-]{1,64}$/.test(name);
}

function _esc(str) {
  return String(str)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}
