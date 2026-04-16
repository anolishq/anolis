export function deriveLaunchBlockReason(status, targetProject) {
  if (!status || typeof status !== "object") {
    return "";
  }

  const running = Boolean(status.running);
  const runningProject =
    typeof status.active_project === "string" && status.active_project !== ""
      ? status.active_project
      : null;

  if (!running || !runningProject) {
    return "";
  }

  if (runningProject === targetProject) {
    return (
      `Launch blocked: runtime is already active for \"${targetProject}\". ` +
      "Use Stop or Restart before launching again."
    );
  }

  return (
    `Launch blocked: runtime is active for \"${runningProject}\". ` +
    `Stop that runtime before launching \"${targetProject}\".`
  );
}
