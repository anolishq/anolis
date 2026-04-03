// validation.js — field-level and system-level validation

/**
 * Validate a single input against rules. Adds/removes inline error message.
 * @param {HTMLInputElement} input
 * @param {{ required?: boolean, min?: number, max?: number,
 *           pattern?: RegExp, patternMsg?: string }} rules
 * @returns {boolean} true if valid
 */
export function validateField(input, rules) {
  const value = input.value.trim();
  let error = null;

  if (rules.required && !value) {
    error = 'Required';
  } else if (rules.min !== undefined && rules.max !== undefined && value !== '') {
    const n = Number(value);
    if (isNaN(n) || n < rules.min || n > rules.max) {
      error = `Must be between ${rules.min} and ${rules.max}`;
    }
  } else if (rules.pattern && value !== '' && !rules.pattern.test(value)) {
    error = rules.patternMsg || 'Invalid format';
  }

  _applyError(input, error);
  return error === null;
}

export function clearFieldError(input) {
  _applyError(input, null);
}

function _applyError(input, message) {
  const parent = input.closest('.form-group') || input.parentElement;
  parent.querySelector('.field-error')?.remove();
  if (message) {
    input.classList.add('input-error');
    const el = document.createElement('span');
    el.className = 'field-error';
    el.textContent = message;
    input.insertAdjacentElement('afterend', el);
  } else {
    input.classList.remove('input-error');
  }
}

/**
 * System-level pre-save validation.
 * @param {object} system
 * @returns {string[]} error messages (empty = valid)
 */
export function validateSystem(system) {
  const errors = [];
  const providers = system.topology?.runtime?.providers ?? [];

  // 1. Duplicate provider IDs
  const seen = new Set();
  const dupes = new Set();
  for (const p of providers) {
    if (seen.has(p.id)) dupes.add(p.id);
    seen.add(p.id);
  }
  if (dupes.size > 0) {
    errors.push(`Duplicate provider IDs: ${[...dupes].join(', ')}`);
  }

  // 2. Port 3002 collision (reserved by System Composer)
  if (system.topology?.runtime?.http_port === 3002) {
    errors.push('Port 3002 is reserved for the System Composer itself.');
  }

  // 3. Duplicate (bus_path, address) across bread/ezo providers
  const busOwnership = {};
  for (const p of providers) {
    if (p.kind !== 'bread' && p.kind !== 'ezo') continue;
    const busPath = system.paths?.providers?.[p.id]?.bus_path ?? '';
    const devices = system.topology?.providers?.[p.id]?.devices ?? [];
    for (const d of devices) {
      const key = `${busPath}::${d.address}`;
      if (busOwnership[key]) {
        errors.push(
          `Address ${d.address} on bus "${busPath}" is claimed by both "${busOwnership[key]}" and "${p.id}".`
        );
      } else {
        busOwnership[key] = p.id;
      }
    }
  }

  // 4. Missing provider topology config
  for (const p of providers) {
    if (p.kind === 'custom') continue;
    if (!system.topology?.providers?.[p.id]) {
      errors.push(`Provider "${p.id}" has no config in topology.providers.`);
    }
  }

  return errors;
}
