/**
 * SSE (Server-Sent Events) module - Real-time event stream
 */

import { CONFIG } from "./config.js";
import { parseSseJsonData } from "./contracts.js";

let eventSource = null;
let sseConnected = false;
let reconnectTimeout = null;
let reconnectAttempts = 0;
let lastEventTime = Date.now();
let stalenessCheckInterval = null;
let eventHandlers = {};

/**
 * Register event handler
 * @param {string} eventType - Event type (e.g., 'mode_change', 'parameter_change')
 * @param {Function} handler - Handler function(data)
 */
export function on(eventType, handler) {
  if (!eventHandlers[eventType]) {
    eventHandlers[eventType] = [];
  }
  eventHandlers[eventType].push(handler);
}

/**
 * Emit event to registered handlers
 */
function emit(eventType, data) {
  const handlers = eventHandlers[eventType] || [];
  handlers.forEach((handler) => {
    try {
      handler(data);
    } catch (err) {
      console.error(`Error in ${eventType} handler:`, err);
    }
  });
}

/**
 * Connect to SSE stream
 */
export function connect() {
  if (eventSource && eventSource.readyState === EventSource.OPEN) {
    console.log("[SSE] Already connected");
    return;
  }

  disconnect();

  const url = `${CONFIG.API_BASE}/v0/events`;
  console.log("[SSE] Connecting to", url);

  try {
    eventSource = new EventSource(url);

    eventSource.onopen = () => {
      console.log("[SSE] Connected");
      sseConnected = true;
      reconnectAttempts = 0;
      lastEventTime = Date.now();
      emit("connection_status", { state: "connected", attempts: 0 });

      // Start staleness check
      if (stalenessCheckInterval) clearInterval(stalenessCheckInterval);
      stalenessCheckInterval = setInterval(() => {
        const idleTime = Date.now() - lastEventTime;
        if (idleTime > 30000 && sseConnected) {
          // 30 seconds without events
          emit("connection_status", { state: "stale", idleTime });
        }
      }, 5000); // Check every 5 seconds
    };

    // State update events
    eventSource.addEventListener("state_update", (event) => {
      try {
        lastEventTime = Date.now();
        const data = parseSseJsonData(event.data);
        emit("state_update", data);
      } catch (err) {
        console.error("[SSE] Failed to parse state_update:", err);
      }
    });

    // Quality change events
    eventSource.addEventListener("quality_change", (event) => {
      try {
        const data = parseSseJsonData(event.data);
        emit("quality_change", data);
      } catch (err) {
        console.error("[SSE] Failed to parse quality_change:", err);
      }
    });

    // Mode change events
    eventSource.addEventListener("mode_change", (event) => {
      try {
        const data = parseSseJsonData(event.data);
        emit("mode_change", data);
      } catch (err) {
        console.error("[SSE] Failed to parse mode_change:", err);
      }
    });

    // Parameter change events
    eventSource.addEventListener("parameter_change", (event) => {
      try {
        lastEventTime = Date.now();
        const data = parseSseJsonData(event.data);
        emit("parameter_change", data);
      } catch (err) {
        console.error("[SSE] Failed to parse parameter_change:", err);
      }
    });

    // BT error events
    eventSource.addEventListener("bt_error", (event) => {
      try {
        lastEventTime = Date.now();
        const data = parseSseJsonData(event.data);
        emit("bt_error", data);
      } catch (err) {
        console.error("[SSE] Failed to parse bt_error:", err);
      }
    });

    // Provider health change events
    eventSource.addEventListener("provider_health_change", (event) => {
      try {
        lastEventTime = Date.now();
        const data = parseSseJsonData(event.data);
        emit("provider_health_change", data);
      } catch (err) {
        console.error("[SSE] Failed to parse provider_health_change:", err);
      }
    });

    // Error handling
    eventSource.onerror = (err) => {
      console.error("[SSE] Error:", err);
      if (eventSource.readyState === EventSource.CLOSED) {
        sseConnected = false;
        reconnectAttempts++;
        emit("connection_status", {
          state: "disconnected",
          attempts: reconnectAttempts,
        });

        // Clear staleness check
        if (stalenessCheckInterval) {
          clearInterval(stalenessCheckInterval);
          stalenessCheckInterval = null;
        }

        // Attempt reconnect with exponential backoff (max 10s)
        const delay = Math.min(
          CONFIG.SSE_RECONNECT_DELAY_MS * reconnectAttempts,
          10000,
        );
        if (reconnectTimeout) clearTimeout(reconnectTimeout);

        emit("connection_status", {
          state: "reconnecting",
          attempts: reconnectAttempts,
          delay_ms: delay,
        });

        reconnectTimeout = setTimeout(() => {
          console.log(
            `[SSE] Attempting reconnect (attempt ${reconnectAttempts})...`,
          );
          connect();
        }, delay);
      }
    };
  } catch (err) {
    console.error("[SSE] Failed to create EventSource:", err);
    emit("connection_status", { state: "error" });
  }
}

/**
 * Disconnect from SSE stream
 */
export function disconnect() {
  if (reconnectTimeout) {
    clearTimeout(reconnectTimeout);
    reconnectTimeout = null;
  }

  if (stalenessCheckInterval) {
    clearInterval(stalenessCheckInterval);
    stalenessCheckInterval = null;
  }

  if (eventSource) {
    eventSource.close();
    eventSource = null;
    sseConnected = false;
    reconnectAttempts = 0;
    console.log("[SSE] Disconnected");
    emit("connection_status", { state: "disconnected", attempts: 0 });
  }
}

/**
 * Get connection status
 */
export function isConnected() {
  return sseConnected;
}
