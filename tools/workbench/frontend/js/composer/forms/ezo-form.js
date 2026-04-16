// forms/ezo-form.js — Provider-EZO typed form

const HEX_RE = /^0x[0-9a-fA-F]{2}$/;
const EZO_DEVICE_TYPES = [
  { type: 'ph',  display: 'pH Sensor'         },
  { type: 'do',  display: 'Dissolved Oxygen'   },
  { type: 'ec',  display: 'Conductivity'        },
  { type: 'orp', display: 'ORP'                },
  { type: 'rtd', display: 'Temperature (RTD)'  },
  { type: 'hum', display: 'Humidity'            },
];

/**
 * @param {HTMLElement} container
 * @param {object} config — live topology.providers[id] object
 * @param {function} onChanged
 */
export function renderEzoForm(container, config, onChanged) {
  container.append(_textGroup('Provider name (optional)', config.provider_name ?? '',
    v => { config.provider_name = v; onChanged(); }
  ));
  container.append(_numberGroup('Query delay (µs)', config.query_delay_us ?? 300000, 0, 2000000, v => { config.query_delay_us = v; onChanged(); }));
  container.append(_numberGroup('Timeout (ms)',      config.timeout_ms    ?? 300,     1,   60000, v => { config.timeout_ms    = v; onChanged(); }));
  container.append(_numberGroup('Retry count',       config.retry_count   ?? 2,       0,      20, v => { config.retry_count   = v; onChanged(); }));

  config.devices = config.devices || [];
  container.append(_buildDeviceList(config, onChanged));
}

function _buildDeviceList(config, onChanged) {
  const section = document.createElement('div');
  section.className = 'device-list-section';
  const h4 = document.createElement('h4'); h4.textContent = 'Devices'; section.append(h4);
  const listEl = document.createElement('div');
  listEl.className = 'device-list'; section.append(listEl);

  function rerender() {
    listEl.innerHTML = '';
    config.devices.forEach((dev, i) => listEl.append(_buildDeviceRow(dev, i, config, onChanged, rerender)));
  }
  rerender();

  const addBtn = document.createElement('button');
  addBtn.type = 'button'; addBtn.className = 'btn-secondary btn-sm'; addBtn.textContent = '+ Add Device';
  addBtn.addEventListener('click', () => {
    const id = _nextId(config.devices, 'ph');
    config.devices.push({ id, type: 'ph', address: '0x63' });
    onChanged(); rerender();
  });
  section.append(addBtn);
  return section;
}

function _buildDeviceRow(dev, index, config, onChanged, rerender) {
  const row = document.createElement('div'); row.className = 'device-row';

  const idInp = document.createElement('input');
  idInp.type = 'text'; idInp.className = 'device-id-input'; idInp.value = dev.id; idInp.spellcheck = false;
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
  for (const dt of EZO_DEVICE_TYPES) {
    const o = document.createElement('option');
    o.value = dt.type; o.textContent = dt.display; if (dt.type === dev.type) o.selected = true;
    typeSel.append(o);
  }
  typeSel.addEventListener('change', () => { dev.type = typeSel.value; onChanged(); });

  const addrInp = document.createElement('input');
  addrInp.type = 'text'; addrInp.className = 'device-addr-input';
  addrInp.placeholder = '0x63'; addrInp.spellcheck = false;
  addrInp.value = dev.address ?? '';
  const addrErr = document.createElement('span');
  addrErr.className = 'field-error'; addrErr.style.display = 'none';
  addrInp.addEventListener('blur', () => {
    const v = addrInp.value.trim();
    if (!HEX_RE.test(v)) {
      addrErr.textContent = 'Must be hex, e.g. 0x63'; addrErr.style.display = 'inline';
      addrInp.classList.add('input-error');
    } else {
      addrErr.style.display = 'none'; addrInp.classList.remove('input-error');
      dev.address = v; onChanged();
    }
  });

  const rmBtn = document.createElement('button');
  rmBtn.type = 'button'; rmBtn.className = 'btn-remove-device'; rmBtn.textContent = '✕';
  rmBtn.addEventListener('click', () => { config.devices.splice(index, 1); onChanged(); rerender(); });

  row.append(idInp, typeSel, addrInp, addrErr, rmBtn);
  return row;
}

function _nextId(devices, prefix) {
  const nums = devices.filter(d => d.id.startsWith(prefix))
    .map(d => parseInt(d.id.slice(prefix.length), 10)).filter(n => !isNaN(n));
  return `${prefix}${nums.length ? Math.max(...nums) + 1 : 0}`;
}

function _textGroup(label, value, onChange) {
  const g = _fmtGroup(label);
  const inp = document.createElement('input');
  inp.type = 'text'; inp.value = value; inp.spellcheck = false;
  inp.addEventListener('input', () => onChange(inp.value));
  g.append(inp); return g;
}

function _numberGroup(label, value, min, max, onChange) {
  const g = _fmtGroup(label);
  const inp = document.createElement('input');
  inp.type = 'number'; inp.value = value; inp.min = min; inp.max = max;
  inp.addEventListener('change', () => { const n = Number(inp.value); if (!isNaN(n)) onChange(n); });
  g.append(inp); return g;
}

function _fmtGroup(label) {
  const g = document.createElement('div'); g.className = 'form-group';
  const lbl = document.createElement('label'); lbl.textContent = label; g.append(lbl);
  return g;
}
