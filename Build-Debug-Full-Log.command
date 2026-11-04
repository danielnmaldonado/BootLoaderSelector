#!/bin/bash
# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
set -u

if ! cd "$(dirname "$0")"; then
  echo "Unable to enter the project directory" >&2
  exit 1
fi
PROJECT_ROOT="$PWD"
LOG_DIR="$PROJECT_ROOT/BuildLogs"
if [ -L "$LOG_DIR" ] || { [ -e "$LOG_DIR" ] && [ ! -d "$LOG_DIR" ]; }; then
  echo "Refusing unsafe build-log directory: $LOG_DIR" >&2
  exit 1
fi
if ! mkdir -p "$LOG_DIR"; then
  echo "Unable to create project build-log directory" >&2
  exit 1
fi
LOG="$LOG_DIR/BootLoaderSelector-Build.log"
if [ -L "$LOG" ] || { [ -e "$LOG" ] && [ ! -f "$LOG" ]; }; then
  echo "Refusing unsafe build log: $LOG" >&2
  exit 1
fi
if [ -e "$LOG" ] && [ -n "$(find "$LOG" -type f -links +1 -print -quit 2>/dev/null)" ]; then
  echo "Refusing multiply-linked build log: $LOG" >&2
  exit 1
fi

# The canonical Release builder owns the shared lock and log. Request debug
# diagnostics from its cleanup path so those diagnostics are appended before
# .BlsBuild.lock is released; this prevents another build from truncating or
# replacing the log while the Debug wrapper is still writing to it.
BLS_DEBUG_DIAGNOSTICS=1 bash ./Build-BootLoaderSelector-Release.command
STATUS=$?

if [ -L "$LOG" ] || [ ! -f "$LOG" ] ||
   [ -n "$(find "$LOG" -type f -links +1 -print -quit 2>/dev/null)" ]; then
  echo "Refusing unsafe build log after Release builder: $LOG" >&2
  exit 1
fi

open -R "$LOG" 2>/dev/null || true
exit "$STATUS"
