# Change Completion Checklist

Use this checklist in implementation PRs for build/dependency/CI policy changes.

## Required

- [ ] Scope implemented exactly as proposed
- [ ] Exit criteria verified
- [ ] Required docs updated
- [ ] Required CI lanes green on target path
- [ ] Rollback path validated
- [ ] Follow-on preconditions confirmed

## Rollout (when applicable)

- [ ] Legacy and new paths run in parallel
- [ ] Rollout start date recorded
- [ ] Consecutive green runs count recorded
- [ ] Minimum gate met (5 green runs)
- [ ] Preferred gate met (10 green runs)
- [ ] Legacy path removal approved

## Traceability

- [ ] Related pins updated (`.ci/dependency-pins.yml`)
- [ ] Rationale/changelog notes included
