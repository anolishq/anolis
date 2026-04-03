// forms/provider-list.js — provider list with add/remove/kind-switch

import { renderSimForm    } from './sim-form.js';
import { renderBreadForm  } from './bread-form.js';
import { renderEzoForm    } from './ezo-form.js';
import { renderCustomForm } from './custom-form.js';

/**
 * Render the Providers section and append it to container.
 * @param {HTMLElement} container
 * @param {object} system   — live system object
 * @param {object} catalog  — catalog from /api/catalog
 * @param {function} onChanged
 */
export function renderProviderList(container, system, catalog, onChanged) {
  const kindMap = {};
  for (const p of catalog.providers) kindMap[p.kind] = p;

  const section = document.createElement('section');
  section.className = 'form-section providers-section';
  section.innerHTML = '<h3>Providers</h3>';

  const listEl = document.createElement('div');
  listEl.className = 'provider-list';
  section.append(listEl);

  const addBtn = document.createElement('button');
  addBtn.type = 'button';
  addBtn.className = 'btn-secondary';
  addBtn.textContent = '+ Add Provider';
  addBtn.addEventListener('click', () => {
    _addProvider(system, 'sim');
    onChanged();
    _rerender();
  });
  section.append(addBtn);
  container.append(section);

  function _rerender() {
    listEl.innerHTML = '';
    system.topology.runtime.providers.forEach((p, i) => {
      listEl.append(_buildRow(p, i, system, kindMap, onChanged, _rerender));
    });
  }
  _rerender();
}

// ---- Helpers ----

function _addProvider(system, kind) {
  const id = _genId(system, kind);
  system.topology.runtime.providers.push({
    id, kind,
    timeout_ms: 5000, hello_timeout_ms: 3000, ready_timeout_ms: 10000,
    restart_policy: { enabled: false },
  });
  system.topology.providers       = system.topology.providers || {};
  system.paths.providers          = system.paths.providers    || {};
  system.topology.providers[id]   = _defaultTopology(kind);
  system.paths.providers[id]      = _defaultPaths(kind);
}

function _genId(system, kind) {
  const existing = (system.topology.runtime.providers || [])
    .filter(p => p.id.startsWith(kind))
    .map(p => parseInt(p.id.slice(kind.length), 10))
    .filter(n => !isNaN(n));
  const next = existing.length ? Math.max(...existing) + 1 : 0;
  return `${kind}${next}`;
}

function _defaultTopology(kind) {
  switch (kind) {
    case 'sim':    return { startup_policy: 'degraded', simulation: { mode: 'non_interacting', tick_rate_hz: 10.0 }, devices: [] };
    case 'bread':  return { provider_name: '', require_live_session: false, query_delay_us: 10000, timeout_ms: 100, retry_count: 2, discovery: { mode: 'manual', addresses: [] }, devices: [] };
    case 'ezo':    return { provider_name: '', query_delay_us: 300000, timeout_ms: 300, retry_count: 2, devices: [] };
    case 'custom': return { args: [] };
    default:       return {};
  }
}

function _defaultPaths(kind) {
  return (kind === 'bread' || kind === 'ezo') ? { executable: '', bus_path: '' } : { executable: '' };
}

function _buildRow(provEntry, index, system, kindMap, onChanged, rerender) {
  const row = document.createElement('div');
  row.className = 'provider-row';

  // --- Header: id input, kind select, remove button ---
  const header = document.createElement('div');
  header.className = 'provider-row-header';

  let currentId = provEntry.id;

  const idInput = document.createElement('input');
  idInput.type = 'text';
  idInput.className = 'provider-id-input';
  idInput.value = currentId;
  idInput.spellcheck = false;
  idInput.title = 'Provider ID';
  idInput.addEventListener('blur', () => {
    const newId = idInput.value.trim();
    if (!newId || newId === currentId) { idInput.value = currentId; return; }
    const allIds = system.topology.runtime.providers.map(p => p.id);
    if (allIds.filter(id => id === newId).length > 0) {
      idInput.value = currentId;
      alert(`Provider ID "${newId}" is already in use.`);
      return;
    }
    _renameId(system, currentId, newId);
    provEntry.id = newId;
    currentId = newId;
    onChanged();
  });

  const kindSel = document.createElement('select');
  kindSel.className = 'provider-kind-select';
  for (const kind of ['sim', 'bread', 'ezo', 'custom']) {
    const opt = document.createElement('option');
    opt.value = kind;
    opt.textContent = kindMap[kind]?.display_name ?? kind;
    if (kind === provEntry.kind) opt.selected = true;
    kindSel.append(opt);
  }
  kindSel.addEventListener('change', () => {
    const newKind = kindSel.value;
    provEntry.kind = newKind;
    system.topology.providers[currentId] = _defaultTopology(newKind);
    system.paths.providers               = system.paths.providers || {};
    system.paths.providers[currentId]    = _defaultPaths(newKind);
    // Refresh typed form and bus-path visibility in-place
    row.querySelector('.provider-typed-form').replaceChildren(
      ..._typedFormNodes(provEntry, system, currentId, onChanged)
    );
    const busGroup = row.querySelector('.bus-path-group');
    busGroup.style.display = (newKind === 'bread' || newKind === 'ezo') ? '' : 'none';
    onChanged();
  });

  const removeBtn = document.createElement('button');
  removeBtn.type = 'button';
  removeBtn.className = 'btn-remove-provider';
  removeBtn.textContent = '✕ Remove';
  removeBtn.addEventListener('click', () => {
    system.topology.runtime.providers.splice(index, 1);
    delete system.topology.providers[currentId];
    if (system.paths.providers) delete system.paths.providers[currentId];
    onChanged();
    rerender();
  });

  header.append(idInput, kindSel, removeBtn);
  row.append(header);

  // --- Timeout fields ---
  const timing = document.createElement('div');
  timing.className = 'provider-timing';
  const lbl = document.createElement('span');
  lbl.className = 'field-group-label';
  lbl.textContent = 'Timeouts';
  timing.append(lbl);
  for (const [key, display] of [['timeout_ms','timeout'],['hello_timeout_ms','hello'],['ready_timeout_ms','ready']]) {
    const label = document.createElement('label');
    label.className = 'inline-label';
    const inp = document.createElement('input');
    inp.type = 'number'; inp.value = provEntry[key] ?? ''; inp.min = 100; inp.max = 120000;
    inp.addEventListener('change', () => {
      const n = Number(inp.value);
      if (!isNaN(n)) { provEntry[key] = n; onChanged(); }
    });
    label.append(inp, document.createTextNode(' ms (' + display + ')'));
    timing.append(label);
  }
  row.append(timing);

  // --- Restart policy ---
  const restartWrap = document.createElement('div');
  restartWrap.className = 'provider-restart';
  const cbLabel = document.createElement('label');
  const cb = document.createElement('input');
  cb.type = 'checkbox';
  cb.checked = provEntry.restart_policy?.enabled ?? false;
  cbLabel.append(cb, document.createTextNode(' Enable restart policy'));
  const restartFields = document.createElement('div');
  restartFields.className = 'restart-fields';
  restartFields.style.display = cb.checked ? '' : 'none';
  cb.addEventListener('change', () => {
    provEntry.restart_policy = provEntry.restart_policy || {};
    provEntry.restart_policy.enabled = cb.checked;
    restartFields.style.display = cb.checked ? '' : 'none';
    onChanged();
  });
  const rp = provEntry.restart_policy || {};
  for (const [key, display, def] of [
    ['max_attempts','max attempts', 5],
    ['backoff_ms',  'backoff (ms)', 1000],
    ['timeout_ms',  'timeout (ms)', 10000],
  ]) {
    const il = document.createElement('label');
    il.className = 'inline-label';
    const inp = document.createElement('input');
    inp.type = 'number'; inp.value = rp[key] ?? def;
    inp.addEventListener('change', () => {
      const n = Number(inp.value);
      if (!isNaN(n)) { provEntry.restart_policy[key] = n; onChanged(); }
    });
    il.append(inp, document.createTextNode(' ' + display));
    restartFields.append(il);
  }
  restartWrap.append(cbLabel, restartFields);
  row.append(restartWrap);

  // --- Executable path ---
  const exeGroup = _fmtGroup('Executable path');
  const exeInp = document.createElement('input');
  exeInp.type = 'text'; exeInp.spellcheck = false;
  exeInp.style.fontFamily = 'monospace';
  exeInp.value = system.paths?.providers?.[currentId]?.executable ?? '';
  exeInp.addEventListener('input', () => {
    system.paths.providers = system.paths.providers || {};
    system.paths.providers[currentId] = system.paths.providers[currentId] || {};
    system.paths.providers[currentId].executable = exeInp.value;
    onChanged();
  });
  exeGroup.append(exeInp);
  row.append(exeGroup);

  // --- Bus path (bread/ezo only) ---
  const busGroup = _fmtGroup('Bus path');
  busGroup.className += ' bus-path-group';
  busGroup.style.display = (provEntry.kind === 'bread' || provEntry.kind === 'ezo') ? '' : 'none';
  const busInp = document.createElement('input');
  busInp.type = 'text'; busInp.spellcheck = false;
  busInp.style.fontFamily = 'monospace';
  busInp.placeholder = '/dev/i2c-1 or mock://name';
  busInp.value = system.paths?.providers?.[currentId]?.bus_path ?? '';
  const busNote = document.createElement('div');
  busNote.className = 'bus-note';
  _updateBusNote(busNote, busInp.value);
  busInp.addEventListener('input', () => {
    system.paths.providers = system.paths.providers || {};
    system.paths.providers[currentId] = system.paths.providers[currentId] || {};
    system.paths.providers[currentId].bus_path = busInp.value;
    _updateBusNote(busNote, busInp.value);
    onChanged();
  });
  busGroup.append(busInp, busNote);
  row.append(busGroup);

  // --- Typed form (collapsible) ---
  const details = document.createElement('details');
  details.className = 'provider-configure';
  details.open = true;
  const summary = document.createElement('summary');
  summary.textContent = 'Configure';
  details.append(summary);
  const typedEl = document.createElement('div');
  typedEl.className = 'provider-typed-form';
  typedEl.replaceChildren(..._typedFormNodes(provEntry, system, currentId, onChanged));
  details.append(typedEl);
  row.append(details);

  return row;
}

function _typedFormNodes(provEntry, system, id, onChanged) {
  const tmp = document.createElement('div');
  const cfg = system.topology.providers?.[id] ?? {};
  switch (provEntry.kind) {
    case 'sim':    renderSimForm   (tmp, cfg, onChanged); break;
    case 'bread':  renderBreadForm (tmp, cfg, onChanged); break;
    case 'ezo':    renderEzoForm   (tmp, cfg, onChanged); break;
    case 'custom': renderCustomForm(tmp, cfg, onChanged); break;
  }
  return [...tmp.children];
}

function _renameId(system, oldId, newId) {
  if (system.topology.providers?.[oldId] !== undefined) {
    system.topology.providers[newId] = system.topology.providers[oldId];
    delete system.topology.providers[oldId];
  }
  if (system.paths?.providers?.[oldId] !== undefined) {
    system.paths.providers[newId] = system.paths.providers[oldId];
    delete system.paths.providers[oldId];
  }
}

function _updateBusNote(el, busPath) {
  el.className = 'bus-note';
  if (!busPath) { el.textContent = ''; return; }
  if (busPath.startsWith('mock://')) {
    el.classList.add('note-success');
    el.textContent = 'Mock bus mode — no hardware required.';
  } else {
    el.classList.add('note-warning');
    el.textContent = 'Live hardware path — requires the bus to be connected.';
  }
}

function _fmtGroup(label) {
  const g = document.createElement('div');
  g.className = 'form-group';
  const lbl = document.createElement('label');
  lbl.textContent = label;
  g.append(lbl);
  return g;
}
