export function evaluateNavigationPrompts({
  dirty,
  currentProject,
  currentWorkspace,
  nextProject,
  nextWorkspace,
  runtimeRunning,
  runningProject,
}) {
  const prompts = [];

  const hasCurrentProject = typeof currentProject === "string" && currentProject !== "";
  const hasNextProject = typeof nextProject === "string" && nextProject !== "";

  const switchingProject = hasCurrentProject && hasNextProject && nextProject !== currentProject;
  const switchingWorkspace =
    hasCurrentProject &&
    hasNextProject &&
    nextProject === currentProject &&
    typeof currentWorkspace === "string" &&
    currentWorkspace !== "" &&
    nextWorkspace !== currentWorkspace;
  const leavingProjectContext = hasCurrentProject && !hasNextProject;

  if (dirty && (switchingProject || switchingWorkspace || leavingProjectContext)) {
    prompts.push({
      id: "unsaved_changes",
      message: "You have unsaved Compose edits. Continue and discard unsaved changes?",
    });
  }

  if (
    hasNextProject &&
    runtimeRunning &&
    typeof runningProject === "string" &&
    runningProject !== "" &&
    runningProject !== nextProject &&
    nextProject !== currentProject
  ) {
    prompts.push({
      id: "switch_while_running",
      message:
        `Project \"${runningProject}\" is currently running. ` +
        `Switching to \"${nextProject}\" will not stop it. Continue?`,
    });
  }

  return prompts;
}

export function describeCrossProjectRunningBanner({
  activeProject,
  runtimeRunning,
  runningProject,
}) {
  if (
    typeof activeProject === "string" &&
    activeProject !== "" &&
    runtimeRunning &&
    typeof runningProject === "string" &&
    runningProject !== "" &&
    runningProject !== activeProject
  ) {
    return `Runtime for \"${runningProject}\" remains active. Launch for \"${activeProject}\" is blocked until you stop the running runtime.`;
  }
  return "";
}
