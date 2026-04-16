// health.js — Polls the runtime's /v0/runtime/status after launch

let _timer = null;
let _startedAt = 0;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Start polling the runtime HTTP API and report status via callback.
 * @param {object} system  - Parsed system.json (for bind/port).
 * @param {function} onStatusChange - Called with 'starting' | 'healthy' | 'unavailable'.
 */
export function startPolling(system, onStatusChange) {
  stopPolling();

  const bind = system?.topology?.runtime?.http_bind || '127.0.0.1';
  const port = system?.topology?.runtime?.http_port || 8080;
  const url = `http://${bind}:${port}/v0/runtime/status`;

  _startedAt = Date.now();
  onStatusChange('starting');

  _timer = setInterval(async () => {
    // First 10 seconds: always report "starting" regardless of poll result
    if (Date.now() - _startedAt < 10_000) {
      onStatusChange('starting');
      return;
    }
    try {
      const res = await fetch(url, { signal: AbortSignal.timeout(2000) });
      onStatusChange(res.ok ? 'healthy' : 'unavailable');
    } catch {
      onStatusChange('unavailable');
    }
  }, 2000);
}

/** Stop polling. Safe to call when not polling. */
export function stopPolling() {
  if (_timer) {
    clearInterval(_timer);
    _timer = null;
  }
}
