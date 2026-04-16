import { renderRuntimeForm } from "./composer/forms/runtime-form.js";
import { renderProviderList } from "./composer/forms/provider-list.js";
import * as launch from "./composer/launch.js";
import { disconnect as disconnectLogs } from "./composer/log-pane.js";
import { stopPolling as stopCommissionPolling } from "./composer/health.js";
import * as operateWorkspace from "./operate-workspace.js";

const state = {
  catalog: null,
  templates: [],
  projects: [],
  runtimeStatus: {},
  projectName: null,
  system: null,
  workspace: null,
  dirty: false,
  currentPath: "/",
  composeRenderedFor: null,
  commissionRunningForCurrent: false,
  lastNavigationId: 0,
};

const elements = {
  homeButton: document.getElementById("btn-home"),
  projectSelector: document.getElementById("project-selector"),
  unsavedIndicator: document.getElementById("unsaved-indicator"),
  workspaceTabs: [...document.querySelectorAll(".tab-btn")],
  runtimeIndicator: document.getElementById("runtime-indicator"),
  globalBanner: document.getElementById("global-banner"),

  workspaceHome: document.getElementById("workspace-home"),
  workspaceCompose: document.getElementById("workspace-compose"),
  workspaceCommission: document.getElementById("workspace-commission"),
  workspaceOperate: document.getElementById("workspace-operate"),

  homeProjectList: document.getElementById("home-project-list"),
  createProjectName: document.getElementById("create-project-name"),
  createProjectTemplate: document.getElementById("create-project-template"),
  createProjectButton: document.getElementById("btn-create-project"),
  createProjectError: document.getElementById("create-project-error"),

  composeAdvisory: document.getElementById("compose-advisory"),
  composeError: document.getElementById("compose-error"),
  composeFormArea: document.getElementById("compose-form-area"),
  saveButton: document.getElementById("btn-save"),

  commissionAdvisory: document.getElementById("commission-advisory"),
};

async function init() {
  _bindUiHandlers();

  await Promise.all([_loadCatalog(), _refreshTemplates(), _refreshProjects(), _refreshStatus()]);

  await _navigateTo(window.location.pathname, {
    replaceHistory: true,
    bypassGuards: true,
  });

  window.setInterval(() => {
    void _refreshStatus();
  }, 2000);

  window.addEventListener("beforeunload", (event) => {
    if (!state.dirty) {
      return;
    }
    event.preventDefault();
    event.returnValue = "";
  });

  window.addEventListener("popstate", () => {
    void _handlePopState();
  });
}

function _bindUiHandlers() {
  elements.homeButton.addEventListener("click", () => {
    void _navigateTo("/");
  });

  elements.projectSelector.addEventListener("change", () => {
    const selected = elements.projectSelector.value;
    if (!selected) {
      void _navigateTo("/");
      return;
    }
    const workspace = state.workspace || "compose";
    void _navigateTo(_projectWorkspacePath(selected, workspace));
  });

  for (const tab of elements.workspaceTabs) {
    tab.addEventListener("click", () => {
      if (!state.projectName) {
        return;
      }
      const workspace = tab.dataset.workspace;
      if (!workspace) {
        return;
      }
      void _navigateTo(_projectWorkspacePath(state.projectName, workspace));
    });
  }

  elements.createProjectButton.addEventListener("click", () => {
    void _createProject();
  });

  elements.saveButton.addEventListener("click", () => {
    void _saveProject();
  });
}

async function _handlePopState() {
  const targetPath = window.location.pathname;
  const ok = await _navigateTo(targetPath, {
    replaceHistory: true,
    historyAlreadySet: true,
  });
  if (!ok) {
    history.pushState({}, "", state.currentPath);
  }
}

async function _navigateTo(
  path,
  { replaceHistory = false, historyAlreadySet = false, bypassGuards = false } = {},
) {
  const route = _parseRoute(path);
  if (!route) {
    if (!historyAlreadySet) {
      history.replaceState({}, "", "/");
    }
    return _navigateTo("/", {
      replaceHistory: true,
      historyAlreadySet: true,
      bypassGuards,
    });
  }

  if (!bypassGuards) {
    const allowed = _confirmNavigation(route);
    if (!allowed) {
      return false;
    }
  }

  const navId = ++state.lastNavigationId;

  if (!historyAlreadySet) {
    if (replaceHistory) {
      history.replaceState({}, "", route.path);
    } else {
      history.pushState({}, "", route.path);
    }
  }

  const projectChanged = route.project !== state.projectName;
  if (projectChanged) {
    _setDirty(false);
  }

  if (projectChanged && route.project) {
    const loaded = await _loadProject(route.project);
    if (!loaded) {
      if (navId !== state.lastNavigationId) {
        return false;
      }
      history.replaceState({}, "", "/");
      await _navigateTo("/", {
        replaceHistory: true,
        historyAlreadySet: true,
        bypassGuards: true,
      });
      return false;
    }
  }

  if (!route.project) {
    _deactivateWorkspace(state.workspace);
    state.projectName = null;
    state.system = null;
    state.workspace = null;
    state.composeRenderedFor = null;
    _showWorkspace("home");
    state.currentPath = "/";
    _syncUi();
    return true;
  }

  if (projectChanged) {
    state.projectName = route.project;
    operateWorkspace.setProjectContext(route.project, state.system);
    state.composeRenderedFor = null;
    state.commissionRunningForCurrent = false;
  }

  const nextWorkspace = route.workspace || "compose";
  if (state.workspace !== nextWorkspace) {
    _deactivateWorkspace(state.workspace);
  }

  state.workspace = nextWorkspace;
  state.currentPath = route.path;
  _syncUi();
  _showWorkspace(nextWorkspace);

  if (nextWorkspace === "compose") {
    _renderComposeWorkspace();
  } else if (nextWorkspace === "commission") {
    _renderCommissionWorkspace();
  } else {
    operateWorkspace.setProjectContext(state.projectName, state.system);
    operateWorkspace.activate();
  }

  return true;
}

function _confirmNavigation(route) {
  const currentProject = state.projectName;
  const nextProject = route.project;
  const nextWorkspace = route.workspace || "compose";
  const switchingProject = currentProject !== null && nextProject !== currentProject;
  const switchingWorkspace =
    currentProject !== null &&
    nextProject === currentProject &&
    state.workspace !== null &&
    nextWorkspace !== state.workspace;
  const leavingProjectContext = currentProject !== null && nextProject === null;

  if (state.dirty && (switchingProject || switchingWorkspace || leavingProjectContext)) {
    const ok = window.confirm(
      "You have unsaved Compose edits. Continue and discard unsaved changes?",
    );
    if (!ok) {
      return false;
    }
  }

  const running = Boolean(state.runtimeStatus?.running);
  const runningProject =
    typeof state.runtimeStatus?.active_project === "string"
      ? state.runtimeStatus.active_project
      : null;

  if (
    nextProject &&
    running &&
    runningProject &&
    runningProject !== nextProject &&
    nextProject !== currentProject
  ) {
    const ok = window.confirm(
      `Project "${runningProject}" is currently running. Switching to "${nextProject}" will not stop it. Continue?`,
    );
    if (!ok) {
      return false;
    }
  }

  return true;
}

function _deactivateWorkspace(workspace) {
  if (workspace === "commission") {
    stopCommissionPolling();
    disconnectLogs();
  } else if (workspace === "operate") {
    operateWorkspace.deactivate();
  }
}

function _showWorkspace(name) {
  const all = [
    elements.workspaceHome,
    elements.workspaceCompose,
    elements.workspaceCommission,
    elements.workspaceOperate,
  ];
  for (const section of all) {
    section.classList.remove("visible");
  }

  if (name === "home") {
    elements.workspaceHome.classList.add("visible");
  } else if (name === "compose") {
    elements.workspaceCompose.classList.add("visible");
  } else if (name === "commission") {
    elements.workspaceCommission.classList.add("visible");
  } else if (name === "operate") {
    elements.workspaceOperate.classList.add("visible");
  }
}

function _syncUi() {
  _renderProjectSelector();
  _renderHomeProjects();
  _renderTemplateOptions();

  elements.unsavedIndicator.style.display = state.dirty ? "inline" : "none";

  for (const tab of elements.workspaceTabs) {
    const workspace = tab.dataset.workspace;
    const active = workspace === state.workspace && Boolean(state.projectName);
    tab.classList.toggle("active", active);
    tab.disabled = !state.projectName;
  }

  const running = Boolean(state.runtimeStatus?.running);
  const runningProject =
    typeof state.runtimeStatus?.active_project === "string"
      ? state.runtimeStatus.active_project
      : null;

  if (running && runningProject) {
    elements.runtimeIndicator.textContent = `Running: ${runningProject}`;
    elements.runtimeIndicator.className = "runtime-indicator running";
  } else {
    elements.runtimeIndicator.textContent = "Stopped";
    elements.runtimeIndicator.className = "runtime-indicator stopped";
  }

  if (
    state.projectName &&
    running &&
    runningProject &&
    runningProject !== state.projectName
  ) {
    _showGlobalBanner(
      `Runtime for "${runningProject}" remains active. Launch for "${state.projectName}" is blocked until you stop the running runtime.`,
    );
  } else {
    _hideGlobalBanner();
  }
}

function _renderProjectSelector() {
  const selector = elements.projectSelector;
  const prior = selector.value;

  selector.innerHTML = "";

  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.textContent = "No project selected";
  selector.appendChild(placeholder);

  for (const project of state.projects) {
    const option = document.createElement("option");
    option.value = project.name;
    option.textContent = project.name;
    selector.appendChild(option);
  }

  if (state.projectName) {
    selector.value = state.projectName;
  } else {
    selector.value = "";
  }

  if (!state.projectName && prior && state.projects.some((p) => p.name === prior)) {
    selector.value = prior;
  }
}

function _renderHomeProjects() {
  const list = elements.homeProjectList;
  list.innerHTML = "";

  if (state.projects.length === 0) {
    list.innerHTML = '<li class="placeholder">No projects yet.</li>';
    return;
  }

  for (const project of state.projects) {
    const li = document.createElement("li");
    li.className = "home-project-item";

    const name = document.createElement("span");
    name.textContent = project.name;

    const openButton = document.createElement("button");
    openButton.type = "button";
    openButton.className = "btn-secondary btn-sm";
    openButton.textContent = "Open";
    openButton.addEventListener("click", () => {
      void _navigateTo(_projectWorkspacePath(project.name, "compose"));
    });

    li.append(name, openButton);
    list.appendChild(li);
  }
}

function _renderTemplateOptions() {
  const select = elements.createProjectTemplate;
  select.innerHTML = "";

  if (state.templates.length === 0) {
    const option = document.createElement("option");
    option.value = "";
    option.textContent = "No templates available";
    select.appendChild(option);
    select.disabled = true;
    return;
  }

  select.disabled = false;
  for (const template of state.templates) {
    const option = document.createElement("option");
    option.value = template.id;
    option.textContent = template.meta?.name || template.id;
    select.appendChild(option);
  }
}

function _renderComposeWorkspace() {
  if (!state.projectName || !state.system) {
    return;
  }

  if (state.composeRenderedFor !== state.projectName) {
    elements.composeFormArea.innerHTML = "";
    const onChanged = () => _setDirty(true);
    renderRuntimeForm(elements.composeFormArea, state.system, onChanged);
    renderProviderList(elements.composeFormArea, state.system, state.catalog, onChanged);
    state.composeRenderedFor = state.projectName;
  }

  elements.composeError.classList.add("hidden");
  elements.composeError.innerHTML = "";

  const running = Boolean(state.runtimeStatus?.running);
  const runningProject =
    typeof state.runtimeStatus?.active_project === "string"
      ? state.runtimeStatus.active_project
      : null;

  if (running && runningProject === state.projectName) {
    elements.composeAdvisory.textContent =
      "Runtime is currently running from this project. Save edits now; changes take effect only after relaunch from Commission.";
    elements.composeAdvisory.classList.remove("hidden");
  } else {
    elements.composeAdvisory.classList.add("hidden");
  }
}

function _renderCommissionWorkspace() {
  if (!state.projectName || !state.system) {
    return;
  }

  const running = Boolean(state.runtimeStatus?.running);
  const runningProject =
    typeof state.runtimeStatus?.active_project === "string"
      ? state.runtimeStatus.active_project
      : null;
  const runningForCurrent = running && runningProject === state.projectName;

  if (running && runningProject && !runningForCurrent) {
    elements.commissionAdvisory.textContent =
      `Runtime is currently running for "${runningProject}". Launch for this project is hard-blocked until stop.`;
    elements.commissionAdvisory.classList.remove("hidden");
  } else {
    elements.commissionAdvisory.classList.add("hidden");
  }

  if (runningForCurrent) {
    launch.restoreRunningState(state.projectName, state.system);
  } else {
    launch.init(state.projectName, state.system);
  }

  state.commissionRunningForCurrent = runningForCurrent;
}

async function _saveProject() {
  if (!state.projectName || !state.system) {
    return;
  }

  elements.composeError.classList.add("hidden");
  elements.composeError.innerHTML = "";

  elements.saveButton.disabled = true;
  elements.saveButton.textContent = "Saving...";

  try {
    const response = await fetch(`/api/projects/${encodeURIComponent(state.projectName)}`, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(state.system),
    });

    const payload = await response.json().catch(() => ({}));

    if (response.ok) {
      _setDirty(false);
      return;
    }

    const details = Array.isArray(payload?.errors) ? payload.errors : [];
    if (details.length > 0) {
      const list = details
        .map((entry) => {
          const path = typeof entry?.path === "string" ? entry.path : "$";
          const message = typeof entry?.message === "string" ? entry.message : "Validation error";
          return `<li><code>${_esc(path)}</code>: ${_esc(message)}</li>`;
        })
        .join("");
      elements.composeError.innerHTML = `<p>${_esc(payload.error || "Save failed")}</p><ul>${list}</ul>`;
    } else {
      elements.composeError.textContent = payload?.error || "Save failed";
    }
    elements.composeError.classList.remove("hidden");
  } catch (err) {
    elements.composeError.textContent = `Save failed: ${_message(err)}`;
    elements.composeError.classList.remove("hidden");
  } finally {
    elements.saveButton.disabled = false;
    elements.saveButton.textContent = "Save";
  }
}

async function _createProject() {
  const name = elements.createProjectName.value.trim();
  const template = elements.createProjectTemplate.value;

  elements.createProjectError.textContent = "";
  elements.createProjectError.classList.add("hidden");

  if (!_validProjectName(name)) {
    elements.createProjectError.textContent =
      "Project name must be 1-64 chars: letters, digits, hyphens, underscores.";
    elements.createProjectError.classList.remove("hidden");
    return;
  }

  if (!template) {
    elements.createProjectError.textContent = "Template is required.";
    elements.createProjectError.classList.remove("hidden");
    return;
  }

  elements.createProjectButton.disabled = true;
  elements.createProjectButton.textContent = "Creating...";

  try {
    const response = await fetch("/api/projects", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name, template }),
    });

    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      elements.createProjectError.textContent = payload?.error || "Failed to create project";
      elements.createProjectError.classList.remove("hidden");
      return;
    }

    elements.createProjectName.value = "";
    await _refreshProjects();
    await _navigateTo(_projectWorkspacePath(name, "compose"), {
      bypassGuards: true,
    });
  } catch (err) {
    elements.createProjectError.textContent = `Failed to create project: ${_message(err)}`;
    elements.createProjectError.classList.remove("hidden");
  } finally {
    elements.createProjectButton.disabled = false;
    elements.createProjectButton.textContent = "Create Project";
  }
}

async function _refreshStatus() {
  try {
    state.runtimeStatus = await _fetchJson("/api/status");
    const operatorUiBase = state.runtimeStatus?.composer?.operator_ui_base;
    if (typeof operatorUiBase === "string" && operatorUiBase.trim() !== "") {
      window.__ANOLIS_COMPOSER__ = {
        ...(window.__ANOLIS_COMPOSER__ || {}),
        operatorUiBase: operatorUiBase.trim(),
      };
    }
    _syncUi();

    const runningForCurrent =
      Boolean(state.runtimeStatus?.running) && state.runtimeStatus?.active_project === state.projectName;
    if (state.workspace === "commission" && runningForCurrent !== state.commissionRunningForCurrent) {
      _renderCommissionWorkspace();
    }
  } catch {
    // Non-fatal; keep prior status.
  }
}

async function _refreshProjects() {
  state.projects = await _fetchJson("/api/projects");
  _syncUi();
}

async function _refreshTemplates() {
  state.templates = await _fetchJson("/api/templates");
  _syncUi();
}

async function _loadCatalog() {
  state.catalog = await _fetchJson("/api/catalog");
}

async function _loadProject(name) {
  try {
    state.system = await _fetchJson(`/api/projects/${encodeURIComponent(name)}`);
    state.projectName = name;
    return true;
  } catch {
    return false;
  }
}

function _parseRoute(path) {
  if (path === "/") {
    return { path: "/", project: null, workspace: null };
  }

  const match = path.match(/^\/projects\/([^/]+)(?:\/(compose|commission|operate))?\/?$/);
  if (!match) {
    return null;
  }

  const project = decodeURIComponent(match[1]);
  const workspace = match[2] || "compose";
  return {
    path: _projectWorkspacePath(project, workspace),
    project,
    workspace,
  };
}

function _projectWorkspacePath(project, workspace) {
  return `/projects/${encodeURIComponent(project)}/${workspace}`;
}

function _setDirty(dirty) {
  state.dirty = Boolean(dirty);
  _syncUi();
}

function _showGlobalBanner(message) {
  elements.globalBanner.textContent = message;
  elements.globalBanner.classList.remove("hidden");
}

function _hideGlobalBanner() {
  elements.globalBanner.classList.add("hidden");
  elements.globalBanner.textContent = "";
}

function _validProjectName(name) {
  return /^[a-zA-Z0-9_-]{1,64}$/.test(name);
}

async function _fetchJson(path) {
  const response = await fetch(path);
  const text = await response.text();
  let data = {};
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}: ${text}`);
      }
      throw new Error(`Invalid JSON from ${path}`);
    }
  }

  if (!response.ok) {
    const message = data?.error || `HTTP ${response.status}`;
    throw new Error(message);
  }

  return data;
}

function _message(err) {
  if (err instanceof Error) {
    return err.message;
  }
  return String(err);
}

function _esc(str) {
  return String(str)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/\"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

void init();
