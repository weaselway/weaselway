#!/bin/bash

set -e -u -x -o pipefail

REPO=mutter
PACKAGE=mutter
PACKAGE_SOURCE=mutter-50.1

BRANCH=50.1-wslg
UPSTREAMTAG=50.1

EXTRA_DEPENDENCIES=freerdp3-dev

SOURCEONLY=${SOURCEONLY:-false}

source _build.sh

build-packages
