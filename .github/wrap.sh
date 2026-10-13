#!/bin/bash
# Runs a workflow step's script like "bash -e", and if it fails also writes the last lines of its output into an error
# annotation, so the reason can be read on the run's summary page (and through the GitHub API) without opening the log.
bash -eo pipefail "$1" 2>&1 | tee /tmp/step.log
rc=${PIPESTATUS[0]}
if [ "$rc" -ne 0 ]; then
  msg=$(tail -n 70 /tmp/step.log | cut -c1-300 | sed 's/%/%25/g' | awk '{printf "%s%%0A", $0}')
  echo "::error title=Step failed (exit code $rc)::$msg"
fi
exit "$rc"
