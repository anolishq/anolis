// forms/custom-form.js — Custom provider minimal form

/**
 * @param {HTMLElement} container
 * @param {object} config — live topology.providers[id] object
 * @param {function} onChanged
 */
export function renderCustomForm(container, config, onChanged) {
  const note = document.createElement('p');
  note.className = 'muted';
  note.style.cssText = 'font-size:12px;margin-bottom:10px;';
  note.textContent = 'Custom provider — configure manually using the CLI args field.';
  container.append(note);

  const g = document.createElement('div');
  g.className = 'form-group';
  const lbl = document.createElement('label');
  lbl.textContent = 'Extra CLI args (one per line)';
  const ta = document.createElement('textarea');
  ta.rows = 4;
  ta.placeholder = '--some-flag\n--config-key value';
  ta.value = (config.args ?? []).join('\n');
  ta.addEventListener('input', () => {
    config.args = ta.value.split('\n').map(s => s.trim()).filter(Boolean);
    onChanged();
  });
  g.append(lbl, ta);
  container.append(g);
}
