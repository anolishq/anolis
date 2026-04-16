// log-pane.js — SSE log stream display

let _evtSource = null;
let _autoScroll = true;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/** Open the log pane and start streaming logs for the given project. */
export function connect(projectName) {
  disconnect();

  const pane = document.getElementById('log-pane');
  pane.classList.add('visible');
  const content = pane.querySelector('.log-content');
  if (!content) return;

  _autoScroll = true;
  content.addEventListener('scroll', _onScroll, { passive: true });

  // Wire the Clear button (clears display only, not the log file)
  const clearBtn = document.getElementById('btn-clear-log');
  if (clearBtn) {
    clearBtn.onclick = () => { content.innerHTML = ''; };
  }

  _evtSource = new EventSource(
    `/api/projects/${encodeURIComponent(projectName)}/logs`
  );
  _evtSource.onmessage = (e) => _appendLine(content, e.data);
  _evtSource.onerror = () => { /* browser retries automatically */ };
}

/** Close the SSE connection. Does not hide the pane (keeps last logs visible). */
export function disconnect() {
  if (_evtSource) {
    _evtSource.close();
    _evtSource = null;
  }
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

function _appendLine(content, text) {
  // Trim to 1000 lines max
  while (content.childElementCount >= 1000) {
    content.removeChild(content.firstChild);
  }
  const line = document.createElement('div');
  line.className = 'log-line';
  line.textContent = text;
  content.appendChild(line);
  if (_autoScroll) {
    content.scrollTop = content.scrollHeight;
  }
}

function _onScroll(evt) {
  const el = evt.target;
  const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 30;
  _autoScroll = atBottom;
}
