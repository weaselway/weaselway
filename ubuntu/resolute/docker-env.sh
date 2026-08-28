#!/usr/bin/env bash
set -xeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

HOST_UID="$(id -u)"
HOST_GID="$(id -g)"
IMAGE="wsl/buildenv:ubuntu-26.04-uid${HOST_UID}"

docker build \
  --build-arg UID="${HOST_UID}" \
  --build-arg GID="${HOST_GID}" \
  -t "${IMAGE}" "${SCRIPT_DIR}"

# Anything before a standalone "--" goes to `docker run` rather than to the
# container, for one-off needs like an extra mount; anything after it is the
# command, as before. With no "--" every argument is the command, so the
# existing invocations are unaffected.
#
#   ./docker-env.sh -v /data:/data --                   shell, with /data too
#   ./docker-env.sh -v /data:/data -- ./build-mesa.sh   and running one thing
EXTRA_DOCKER_ARGS=()
for arg in "$@"; do
  [ "$arg" = "--" ] || continue

  while [ "$1" != "--" ]; do
    EXTRA_DOCKER_ARGS+=("$1")
    shift
  done
  shift
  break
done

DOCKER_RUN=(docker run --rm -it
  --user "${HOST_UID}:${HOST_GID}"
  -v "$PWD:/work"
  -w /work
  "${EXTRA_DOCKER_ARGS[@]}"
  "${IMAGE}")

if [ $# -gt 0 ]; then
  exec "${DOCKER_RUN[@]}" "$@"
fi

"${DOCKER_RUN[@]}"
