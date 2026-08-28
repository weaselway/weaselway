#!/bin/bash

set -e -u -x -o pipefail

REPO=mesa
PACKAGE=mesa
PACKAGE_SOURCE=mesa-26.0.8

BRANCH=mesa-26.0.8-wsl
UPSTREAMTAG=mesa-26.0.8

EXTRA_DEPENDENCIES=""

SOURCEONLY=${SOURCEONLY:-false}

source _build.sh

build-packages
