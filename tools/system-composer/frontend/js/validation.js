// validation.js — field-level validation helpers

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
