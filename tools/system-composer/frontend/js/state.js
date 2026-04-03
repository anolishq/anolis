// state.js — in-memory project state for the System Composer UI

let _current = null; // { name: string, system: object }
let _dirty = false;
const _listeners = [];

function _deepClone(obj) {
  return JSON.parse(JSON.stringify(obj));
}

export function setProject(name, system) {
  _current = (name !== null && system !== null)
    ? { name, system: _deepClone(system) }
    : null;
  _dirty = false;
  _notify();
}

export function getProject() {
  return _current;
}

/** Returns the live (mutable) system object. Forms mutate this directly. */
export function getSystem() {
  return _current ? _current.system : null;
}

export function isDirty() {
  return _dirty;
}

export function markDirty() {
  if (!_dirty) {
    _dirty = true;
    _notify();
  }
}

export function markClean() {
  _dirty = false;
  _notify();
}

export function onStateChange(fn) {
  _listeners.push(fn);
}

function _notify() {
  _listeners.forEach(fn => fn());
}
