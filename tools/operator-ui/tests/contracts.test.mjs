import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import {
  extractCapabilities,
  extractDeviceStateValues,
  extractDevices,
  extractMode,
  extractParameters,
  parseSseFrame,
  parseSseJsonData,
} from "../js/contracts.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const repoRoot = path.resolve(__dirname, "../../..");
const fixtureRoot = path.join(repoRoot, "tests/contracts/runtime-http/examples");

function loadJsonFixture(name) {
  const fixturePath = path.join(fixtureRoot, name);
  return JSON.parse(fs.readFileSync(fixturePath, "utf8"));
}

function loadTextFixture(name) {
  const fixturePath = path.join(fixtureRoot, name);
  return fs.readFileSync(fixturePath, "utf8");
}

test("devices fixture yields operator-ui device list", () => {
  const payload = loadJsonFixture("devices.200.json");
  const devices = extractDevices(payload);
  assert.ok(Array.isArray(devices));
  assert.ok(devices.length > 0);
  assert.equal(typeof devices[0].provider_id, "string");
  assert.equal(typeof devices[0].device_id, "string");
});

test("device capabilities fixture yields normalized signals/functions arrays", () => {
  const payload = loadJsonFixture("device-capabilities.200.json");
  const capabilities = extractCapabilities(payload);
  assert.ok(Array.isArray(capabilities.signals));
  assert.ok(Array.isArray(capabilities.functions));
  assert.ok(capabilities.functions.length > 0);
  assert.equal(typeof capabilities.functions[0].function_id, "number");
});

test("device state fixture normalizes timestamp_epoch_ms to timestamp_ms", () => {
  const payload = loadJsonFixture("device-state.200.json");
  const values = extractDeviceStateValues(payload);
  assert.ok(Array.isArray(values));
  assert.ok(values.length > 0);
  assert.equal(values[0].timestamp_ms, payload.values[0].timestamp_epoch_ms);
});

test("mode and parameters fixtures are parsed for automation panel", () => {
  const modePayload = loadJsonFixture("mode.get.200.json");
  const parametersPayload = loadJsonFixture("parameters.get.200.json");
  const mode = extractMode(modePayload);
  const parameters = extractParameters(parametersPayload);
  assert.equal(mode, "MANUAL");
  assert.ok(Array.isArray(parameters));
  assert.ok(parameters.some((entry) => entry.name === "feed_enable"));
});

test("sse fixture frame parses and event JSON is decoded", () => {
  const sseFrameText = loadTextFixture("events.200.txt");
  const frame = parseSseFrame(sseFrameText);
  assert.equal(frame.event, "state_update");
  assert.equal(frame.id, "123");
  const payload = parseSseJsonData(frame.data);
  assert.equal(payload.provider_id, "sim0");
  assert.equal(payload.device_id, "tempctl0");
  assert.equal(payload.timestamp_ms, 1710000000123);
});
