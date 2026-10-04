#!/usr/bin/env bash

# Put a shortcut on the Windows desktop that opens the session: a double click
# starts this distro if it is not running, the session if there is none, and
# the viewer.
#
# Usage: install-viewer-link.sh [NAME]
#
# NAME is the shortcut's name, "Weaselway" if not given.

set -eu -o pipefail

NAME="${1:-Weaselway}"

if [ -z "${WSL_DISTRO_NAME:-}" ]; then
    echo "error: WSL_DISTRO_NAME is not set -- run this from a shell WSL started" >&2
    exit 1
fi

POWERSHELL=/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
if [ ! -x "${POWERSHELL}" ]; then
    POWERSHELL="$(command -v powershell.exe || true)"
fi
if [ -z "${POWERSHELL}" ]; then
    echo "error: cannot find powershell.exe" >&2
    exit 1
fi

# What the shortcut runs inside the distro. A login shell, because wsl.exe
# starts the command without one and PATH would lack the system's programs;
# by its full path for the same reason. start-session returns once the
# session is up.
BASH_PATH="$(command -v bash)"
if [ -e /run/current-system/sw/bin/bash ]; then
    # Not the store path: that one goes away with an update.
    BASH_PATH=/run/current-system/sw/bin/bash
fi
COMMAND='systemctl is-active --quiet weaselway-session || start-session; exec start-viewer'

# Single quotes are PowerShell's literal strings; a quote inside is doubled.
quote() {
    printf "'%s'" "${1//\'/\'\'}"
}

# The window is wsl.exe's console, which has to stay for as long as the viewer
# runs; 7 starts it minimized. The icon is wsl.exe's own.
SCRIPT="
\$ErrorActionPreference = 'Stop'
\$path = Join-Path ([Environment]::GetFolderPath('Desktop')) ($(quote "${NAME}") + '.lnk')
\$link = (New-Object -ComObject WScript.Shell).CreateShortcut(\$path)
\$link.TargetPath = Join-Path \$env:SystemRoot 'System32\\wsl.exe'
\$link.Arguments = '-d ' + $(quote "${WSL_DISTRO_NAME}") + ' --cd ~ -- ' + $(quote "${BASH_PATH}") + ' -lc ' + $(quote "\"${COMMAND}\"")
\$link.WindowStyle = 7
\$link.Description = 'The ' + $(quote "${WSL_DISTRO_NAME}") + ' desktop'
\$link.Save()
Write-Output \$path
"

# From a Windows directory: powershell.exe warns about a UNC working directory.
cd /mnt/c
LINK="$("${POWERSHELL}" -NoProfile -NonInteractive -Command "${SCRIPT}" | tr -d '\r')"

echo "Created ${LINK}"
