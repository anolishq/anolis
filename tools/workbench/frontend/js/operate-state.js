export function deriveOperateAvailability(statusPayload, projectName) {
  const running = Boolean(statusPayload?.running);
  const runningProject =
    typeof statusPayload?.active_project === "string" ? statusPayload.active_project : "";

  if (!running) {
    return {
      available: false,
      reason: "stopped",
      message: "Runtime is stopped. Start runtime from Commission to operate this project.",
      runningProject,
    };
  }

  if (runningProject !== projectName) {
    return {
      available: false,
      reason: "different_project",
      message: `Runtime is running for project \"${runningProject}\". Stop it before operating \"${projectName}\".`,
      runningProject,
    };
  }

  return {
    available: true,
    reason: "available",
    message: "",
    runningProject,
  };
}

export function normalizeProviderHealthQuality(quality) {
  const value = String(quality || "UNKNOWN").toUpperCase();
  if (value === "OK" || value === "READY" || value === "AVAILABLE") {
    return "OK";
  }
  if (value === "FAULT") {
    return "FAULT";
  }
  if (value === "UNAVAILABLE" || value === "STALE") {
    return "UNAVAILABLE";
  }
  return "UNKNOWN";
}
