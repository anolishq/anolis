// forms/runtime-form.js — runtime configuration form section

/**
 * Renders the Runtime section and appends it to container.
 * @param {HTMLElement} container
 * @param {object} system  — live system object (mutated directly by form inputs)
 * @param {function} onChanged — called on any field change
 */
export function renderRuntimeForm(container, system, onChanged) {
  const rt    = system.topology.runtime;
  const paths = system.paths;
  _normalizeRuntimeConfig(rt);

  const section = document.createElement('section');
  section.className = 'form-section';
  section.innerHTML = '<h3>Runtime</h3>';

  // --- field helpers ---

  function _text(label, getValue, setValue, opts = {}) {
    const g = _group(label);
    const inp = document.createElement('input');
    inp.type = 'text';
    inp.value = getValue() ?? '';
    if (opts.placeholder) inp.placeholder = opts.placeholder;
    if (opts.mono) inp.style.fontFamily = 'monospace';
    inp.spellcheck = false;
    inp.addEventListener('input', () => { setValue(inp.value); onChanged(); });
    g.append(inp);
    if (opts.note) {
      const n = document.createElement('span');
      n.className = 'field-note';
      n.textContent = opts.note;
      g.append(n);
    }
    (opts.target || section).append(g);
  }

  function _number(label, getValue, setValue, min, max, opts = {}) {
    const g = _group(label);
    const inp = document.createElement('input');
    inp.type = 'number';
    inp.value = getValue() ?? '';
    inp.min = min; inp.max = max;
    inp.addEventListener('change', () => {
      const n = Number(inp.value);
      if (!isNaN(n)) { setValue(n); onChanged(); }
    });
    g.append(inp);
    if (opts.note) {
      const n = document.createElement('span');
      n.className = 'field-note';
      n.textContent = opts.note;
      g.append(n);
    }
    (opts.target || section).append(g);
  }

  function _select(label, getValue, setValue, options) {
    const g = _group(label);
    const sel = document.createElement('select');
    for (const [v, d] of options) {
      const o = document.createElement('option');
      o.value = v; o.textContent = d;
      if (v === getValue()) o.selected = true;
      sel.append(o);
    }
    sel.addEventListener('change', () => { setValue(sel.value); onChanged(); });
    g.append(sel);
    section.append(g);
  }

  function _checkbox(label, getValue, setValue) {
    const g = document.createElement('div');
    g.className = 'form-group form-group-inline';
    const lbl = document.createElement('label');
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.checked = !!getValue();
    cb.addEventListener('change', () => { setValue(cb.checked); onChanged(); });
    lbl.append(cb, document.createTextNode(' ' + label));
    g.append(lbl);
    section.append(g);
  }

  function _textarea(label, getValue, setValue, opts = {}) {
    const g = _group(label);
    const ta = document.createElement('textarea');
    ta.rows = 3;
    ta.value = (getValue() ?? []).join('\n');
    if (opts.placeholder) ta.placeholder = opts.placeholder;
    ta.addEventListener('input', () => {
      setValue(ta.value.split('\n').map(s => s.trim()).filter(Boolean));
      onChanged();
    });
    g.append(ta);
    section.append(g);
  }

  // --- render fields ---

  _text  ('Runtime name',           () => rt.name,                v => { rt.name = v; });
  _number('HTTP port',              () => rt.http_port,           v => { rt.http_port = v; },           1, 65535);
  _text  ('HTTP bind address',      () => rt.http_bind,           v => { rt.http_bind = v; }, { mono: true });
  _textarea('CORS origins (one per line)', () => rt.cors_origins, v => { rt.cors_origins = v; },
    { placeholder: 'http://localhost:3000' });
  _checkbox('CORS allow credentials', () => rt.cors_allow_credentials, v => { rt.cors_allow_credentials = v; });
  _number('Shutdown timeout (ms)',  () => rt.shutdown_timeout_ms, v => { rt.shutdown_timeout_ms = v; }, 500,  30000);
  _number('Startup timeout (ms)',   () => rt.startup_timeout_ms,  v => { rt.startup_timeout_ms  = v; }, 5000, 300000);
  _number('Polling interval (ms)',  () => rt.polling_interval_ms, v => { rt.polling_interval_ms = v; }, 100,  10000);
  _select('Log level',              () => rt.log_level,           v => { rt.log_level = v; },
    [['debug','debug'],['info','info'],['warn','warn'],['error','error']]);

  const telemetryFields = document.createElement('div');
  telemetryFields.className = 'telemetry-fields';
  telemetryFields.style.display = rt.telemetry?.enabled ? '' : 'none';

  _checkbox('Telemetry enabled', () => rt.telemetry?.enabled, v => {
    rt.telemetry = rt.telemetry || {};
    rt.telemetry.enabled = v;
    telemetryFields.style.display = v ? '' : 'none';
  });
  section.append(telemetryFields);

  _text('InfluxDB URL',
    () => rt.telemetry?.influxdb?.url ?? '',
    v  => { _ensureInflux(rt).url = v.trim(); },
    { mono: true, target: telemetryFields, placeholder: 'http://localhost:8086' }
  );
  _text('InfluxDB org',
    () => rt.telemetry?.influxdb?.org ?? '',
    v  => { _ensureInflux(rt).org = v.trim(); },
    { target: telemetryFields }
  );
  _text('InfluxDB bucket',
    () => rt.telemetry?.influxdb?.bucket ?? '',
    v  => { _ensureInflux(rt).bucket = v.trim(); },
    { target: telemetryFields }
  );
  _text('InfluxDB token',
    () => rt.telemetry?.influxdb?.token ?? '',
    v  => { _ensureInflux(rt).token = v; },
    { mono: true, target: telemetryFields, note: 'Stored in system.json for the checked-in dev telemetry profile.' }
  );
  _number('Influx batch size',
    () => rt.telemetry?.influxdb?.batch_size,
    v  => { _ensureInflux(rt).batch_size = v; },
    1, 100000,
    { target: telemetryFields }
  );
  _number('Influx flush interval (ms)',
    () => rt.telemetry?.influxdb?.flush_interval_ms,
    v  => { _ensureInflux(rt).flush_interval_ms = v; },
    1, 600000,
    { target: telemetryFields }
  );

  _checkbox('Automation enabled',   () => rt.automation_enabled,  v => { rt.automation_enabled = v; });
  _text('Behavior tree path',
    () => rt.behavior_tree_path ?? '',
    v  => { rt.behavior_tree_path = v.trim() || null; },
    { mono: true, placeholder: 'systems/<name>/behaviors/main.xml', note: 'Optional. Composer stores behavior_tree_path here and renders runtime YAML with automation.behavior_tree.' }
  );
  _text('Runtime executable path',
    () => paths.runtime_executable,
    v  => { paths.runtime_executable = v; },
    { mono: true, note: 'Default assumes CMake dev-release preset. Change if your build output is elsewhere.' }
  );

  container.append(section);
}

function _group(label) {
  const g = document.createElement('div');
  g.className = 'form-group';
  const lbl = document.createElement('label');
  lbl.textContent = label;
  g.append(lbl);
  return g;
}

function _normalizeRuntimeConfig(rt) {
  if (rt.cors_allow_credentials === undefined) {
    rt.cors_allow_credentials = false;
  }

  if (rt.telemetry_enabled !== undefined) {
    rt.telemetry = rt.telemetry || {};
    if (rt.telemetry.enabled === undefined) {
      rt.telemetry.enabled = !!rt.telemetry_enabled;
    }
    delete rt.telemetry_enabled;
  }

  if (!rt.telemetry || typeof rt.telemetry !== 'object') {
    rt.telemetry = { enabled: false };
  }
  if (rt.telemetry.enabled === undefined) {
    rt.telemetry.enabled = false;
  }
}

function _ensureInflux(rt) {
  rt.telemetry = rt.telemetry || {};
  rt.telemetry.influxdb = rt.telemetry.influxdb || {};
  return rt.telemetry.influxdb;
}
