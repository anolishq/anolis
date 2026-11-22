#!/usr/bin/env bash
# Open, or add to, the issue that says a workflow failed on main (#504).
#
# Lanes that run on main only (or only for some pull requests) can fail
# without any pull request showing it. Each failing workflow keeps one open
# issue labelled ci-main, titled "main: <workflow> failed"; a later failure
# adds a comment to it rather than opening another. Close the issue when main
# is green again.
#
# Needs GH_TOKEN (issues: write) and GH_REPO, plus WORKFLOW, RUN_URL and SHA.
set -euo pipefail

: "${WORKFLOW:?WORKFLOW is required}"
: "${RUN_URL:?RUN_URL is required}"
: "${SHA:?SHA is required}"

label="ci-main"
title="main: ${WORKFLOW} failed"
body="${WORKFLOW} failed on main at ${SHA}: ${RUN_URL}"

# Creating a label that exists fails; that is the only failure ignored here.
gh label create "${label}" --color B60205 \
    --description "A workflow failed on main" >/dev/null 2>&1 || true

# A failed listing must not read as "no open issue". It is retried; if it
# never answers, the failure is reported anyway in a new issue that says it
# may duplicate an open one: an unreported failure on main is worse than a
# duplicate issue (review of #901).
open_issues=""
listed=0
for attempt in 1 2 3; do
    if open_issues=$(gh issue list --state open --label "${label}" --limit 100 \
        --json number,title --jq '.[] | "\(.number)\t\(.title)"'); then
        listed=1
        break
    fi
    echo "Listing the open ${label} issues failed (attempt ${attempt} of 3)" >&2
    [[ ${attempt} -lt 3 ]] && sleep "${LIST_RETRY_DELAY:-10}"
done
if [[ ${listed} -eq 0 ]]; then
    open_issues=""
    body="${body}

The open ${label} issues could not be listed, so this may duplicate one."
fi

existing=""
while IFS=$'\t' read -r number issue_title; do
    if [[ "${issue_title}" == "${title}" ]]; then
        existing="${number}"
        break
    fi
done <<< "${open_issues}"

if [[ -n "${existing}" ]]; then
    gh issue comment "${existing}" --body "${body}"
    echo "Added the failure to #${existing}"
else
    gh issue create --title "${title}" --label "${label}" \
        --body "${body}

Lanes that run only on main, or only for some pull requests, report here. Close this issue once main is green again."
    echo "Opened an issue for the failure"
fi
