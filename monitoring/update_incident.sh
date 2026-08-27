#!/usr/bin/env bash
set -Eeuo pipefail

readonly outcome=${1:?probe outcome is required}
readonly incident_title='[uptime] watering.sibex.zip is unavailable'
readonly run_url="${GITHUB_SERVER_URL}/${GITHUB_REPOSITORY}/actions/runs/${GITHUB_RUN_ID}"

case "$outcome" in
  success | failure) ;;
  *) printf 'unsupported probe outcome: %s\n' "$outcome" >&2; exit 2 ;;
esac

issue_number=$(
  gh issue list \
    --repo "$GITHUB_REPOSITORY" \
    --state open \
    --limit 100 \
    --json number,title \
    --jq '.[] | select(.title == "[uptime] watering.sibex.zip is unavailable") | .number' \
    | head -n 1
)

if [[ $outcome == failure ]]; then
  if [[ -z $issue_number ]]; then
    gh issue create \
      --repo "$GITHUB_REPOSITORY" \
      --title "$incident_title" \
      --assignee "$GITHUB_REPOSITORY_OWNER" \
      --body "The external production monitor failed after three attempts. Review the [failed workflow run]($run_url) for the safe probe diagnostics. This issue will close automatically after the service recovers."
    printf 'incident_action=opened\n'
  else
    printf 'incident_action=already_open\n'
  fi
else
  if [[ -n $issue_number ]]; then
    gh issue comment "$issue_number" \
      --repo "$GITHUB_REPOSITORY" \
      --body "The external production monitor recovered. Verified by [workflow run]($run_url)."
    gh issue close "$issue_number" --repo "$GITHUB_REPOSITORY" --reason completed
    printf 'incident_action=closed_after_recovery\n'
  else
    printf 'incident_action=none\n'
  fi
fi
