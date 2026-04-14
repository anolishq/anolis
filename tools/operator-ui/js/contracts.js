/**
 * Runtime HTTP contract normalizers for Operator UI.
 *
 * These helpers are intentionally pure (no DOM, no network) so they can be
 * fixture-tested against tests/contracts/runtime-http/examples.
 */

function asObject(value) {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value
    : {};
}

function asArray(value) {
  return Array.isArray(value) ? value : [];
}

function toFiniteNumber(value) {
  const num = Number(value);
  return Number.isFinite(num) ? num : null;
}

export function extractDevices(payload) {
  const root = asObject(payload);
  return asArray(root.devices);
}

export function extractCapabilities(payload) {
  const root = asObject(payload);
  const capabilities = asObject(root.capabilities);
  return {
    ...capabilities,
    signals: asArray(capabilities.signals),
    functions: asArray(capabilities.functions),
  };
}

export function normalizeStateSignal(signal) {
  const source = asObject(signal);
  const tsMs =
    toFiniteNumber(source.timestamp_ms) ??
    toFiniteNumber(source.timestamp_epoch_ms) ??
    0;
  return {
    ...source,
    timestamp_ms: tsMs,
  };
}

export function extractDeviceStateValues(payload) {
  const root = asObject(payload);
  return asArray(root.values).map((entry) => normalizeStateSignal(entry));
}

export function extractParameters(payload) {
  const root = asObject(payload);
  return asArray(root.parameters);
}

export function extractMode(payload) {
  const root = asObject(payload);
  return typeof root.mode === "string" ? root.mode : null;
}

export function parseSseJsonData(data) {
  if (typeof data !== "string") {
    throw new Error("SSE data payload must be a string");
  }
  return JSON.parse(data);
}

export function parseSseFrame(frameText) {
  const record = { event: "", id: "", data: "" };
  const lines = String(frameText).split(/\r?\n/);
  for (const line of lines) {
    if (line.startsWith("event:")) {
      record.event = line.slice("event:".length).trim();
      continue;
    }
    if (line.startsWith("id:")) {
      record.id = line.slice("id:".length).trim();
      continue;
    }
    if (line.startsWith("data:")) {
      const value = line.slice("data:".length).trim();
      record.data = record.data ? `${record.data}\n${value}` : value;
    }
  }
  return record;
}
