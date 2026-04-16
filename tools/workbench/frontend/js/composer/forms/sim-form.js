// forms/sim-form.js — Provider-Sim typed form

const SIM_DEVICE_TYPES = [
  { type: 'tempctl',     display: 'Temperature Controller', fields: [{ key: 'initial_temp', label: 'Initial temp (°C)', default: 25.0 }] },
  { type: 'motorctl',    display: 'Motor Controller',       fields: [{ key: 'max_speed',    label: 'Max speed (RPM)',  default: 3000.0 }] },
  { type: 'relayio',     display: 'Relay I/O',              fields: [] },
  { type: 'analogsensor',display: 'Analog Sensor',          fields: [] },
];

/**
 * @param {HTMLElement} container
 * @param {object} config — live topology.providers[id] object
 * @param {function} onChanged
 */
export function renderSimForm(container, config, onChanged) {
  _normalizeSimConfig(config);

  // startup_policy
  container.append(_selectGroup('Startup policy',
    config.startup_policy ?? 'degraded',
    [['strict','strict'],['degraded','degraded']],
    v => { config.startup_policy = v; onChanged(); }
  ));

  // simulation_mode — guard for unsupported 'sim' mode
  const simMode = config.simulation_mode ?? 'non_interacting';
  if (simMode === 'sim') {
    const badge = document.createElement('div');
    badge.className = 'note-warning';
    badge.style.cssText = 'font-size:12px;padding:6px 10px;border-radius:4px;margin-bottom:10px;';
    badge.textContent = '⚠ mode=sim requires manual physics_config_path — not editable in this version.';
    container.append(badge);
  } else {
    const tickRow = _numberGroup('Tick rate (Hz)',
      config.tick_rate_hz ?? 10.0, 1, 100,
      v => { config.tick_rate_hz = v; onChanged(); }
    );
    tickRow.style.display = simMode === 'inert' ? 'none' : '';

    container.append(_selectGroup('Simulation mode',
      simMode,
      [['inert','inert'],['non_interacting','non_interacting']],
      v => {
        config.simulation_mode = v;
        tickRow.style.display = v === 'inert' ? 'none' : '';
        onChanged();
      }
    ));
    container.append(tickRow);
  }

  // Device list
  config.devices = config.devices || [];
  container.append(_buildSimDeviceList(config, onChanged));
}

function _buildSimDeviceList(config, onChanged) {
  const section = document.createElement('div');
  section.className = 'device-list-section';
  const h4 = document.createElement('h4');
  h4.textContent = 'Devices';
  section.append(h4);

  const listEl = document.createElement('div');
  listEl.className = 'device-list';
  section.append(listEl);

  function rerender() {
    listEl.innerHTML = '';
    config.devices.forEach((dev, i) => {
      listEl.append(_buildSimDeviceRow(dev, i, config, onChanged, rerender));
    });
    // chaos_control read-only badge
    const badge = document.createElement('div');
    badge.className = 'chaos-badge muted';
    badge.textContent = 'ℹ chaos_control — Fault injection device — always included by the provider, not configurable here.';
    listEl.append(badge);
  }
  rerender();

  const addBtn = document.createElement('button');
  addBtn.type = 'button';
  addBtn.className = 'btn-secondary btn-sm';
  addBtn.textContent = '+ Add Device';
  addBtn.addEventListener('click', () => {
    const id = _nextId(config.devices, 'tempctl');
    config.devices.push({ id, type: 'tempctl', initial_temp: 25.0 });
    onChanged(); rerender();
  });
  section.append(addBtn);
  return section;
}

function _buildSimDeviceRow(dev, index, config, onChanged, rerender) {
  const row = document.createElement('div');
  row.className = 'device-row';

  const idInp = document.createElement('input');
  idInp.type = 'text'; idInp.className = 'device-id-input';
  idInp.value = dev.id; idInp.spellcheck = false;
  idInp.addEventListener('blur', () => {
    const v = idInp.value.trim();
    if (!v) { idInp.value = dev.id; return; }
    if (config.devices.some((d, i) => d.id === v && i !== index)) {
      idInp.value = dev.id; alert(`Device ID "${v}" is already in use.`); return;
    }
    dev.id = v; onChanged();
  });

  const typeSel = document.createElement('select');
  typeSel.className = 'device-type-select';
  for (const dt of SIM_DEVICE_TYPES) {
    const o = document.createElement('option');
    o.value = dt.type; o.textContent = dt.display;
    if (dt.type === dev.type) o.selected = true;
    typeSel.append(o);
  }
  typeSel.addEventListener('change', () => {
    // Clear old type-specific fields
    const old = SIM_DEVICE_TYPES.find(d => d.type === dev.type);
    if (old) old.fields.forEach(f => delete dev[f.key]);
    dev.type = typeSel.value;
    const neo = SIM_DEVICE_TYPES.find(d => d.type === dev.type);
    if (neo) neo.fields.forEach(f => { dev[f.key] = f.default; });
    onChanged(); rerender();
  });

  const rmBtn = document.createElement('button');
  rmBtn.type = 'button'; rmBtn.className = 'btn-remove-device'; rmBtn.textContent = '✕';
  rmBtn.addEventListener('click', () => { config.devices.splice(index, 1); onChanged(); rerender(); });

  row.append(idInp, typeSel, rmBtn);

  // Type-specific extra fields
  const dt = SIM_DEVICE_TYPES.find(d => d.type === dev.type);
  if (dt) {
    for (const f of dt.fields) {
      const il = document.createElement('label');
      il.className = 'inline-label';
      const inp = document.createElement('input');
      inp.type = 'number'; inp.value = dev[f.key] ?? f.default;
      inp.addEventListener('change', () => {
        const n = Number(inp.value);
        if (!isNaN(n)) { dev[f.key] = n; onChanged(); }
      });
      il.append(document.createTextNode(f.label + ': '), inp);
      row.append(il);
    }
  }
  return row;
}

// ---- shared helpers ----

function _nextId(devices, prefix) {
  const nums = devices.filter(d => d.id.startsWith(prefix))
    .map(d => parseInt(d.id.slice(prefix.length), 10)).filter(n => !isNaN(n));
  return `${prefix}${nums.length ? Math.max(...nums) + 1 : 0}`;
}

function _selectGroup(label, value, options, onChange) {
  const g = _fmtGroup(label);
  const sel = document.createElement('select');
  for (const [v, d] of options) {
    const o = document.createElement('option');
    o.value = v; o.textContent = d; if (v === value) o.selected = true;
    sel.append(o);
  }
  sel.addEventListener('change', () => onChange(sel.value));
  g.append(sel);
  return g;
}

function _numberGroup(label, value, min, max, onChange) {
  const g = _fmtGroup(label);
  const inp = document.createElement('input');
  inp.type = 'number'; inp.value = value; inp.min = min; inp.max = max;
  inp.addEventListener('change', () => { const n = Number(inp.value); if (!isNaN(n)) onChange(n); });
  g.append(inp);
  return g;
}

function _fmtGroup(label) {
  const g = document.createElement('div');
  g.className = 'form-group';
  const lbl = document.createElement('label');
  lbl.textContent = label;
  g.append(lbl);
  return g;
}

function _normalizeSimConfig(config) {
  if (config.simulation && typeof config.simulation === 'object') {
    if (config.simulation_mode === undefined && config.simulation.mode !== undefined) {
      config.simulation_mode = config.simulation.mode;
    }
    if (config.tick_rate_hz === undefined && config.simulation.tick_rate_hz !== undefined) {
      config.tick_rate_hz = config.simulation.tick_rate_hz;
    }
    delete config.simulation;
  }
}
