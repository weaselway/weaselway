#!/usr/bin/env bash

# Answers the kernel's sysfs firmware requests, run by udev. udev passes
# FIRMWARE (the file name) and DEVPATH (the sysfs request). Looks in the
# firmware the Bluetooth modules came with first, then in the distro's.

REQUEST="/sys${DEVPATH}"

for DIR in /opt/weaselway/bluetooth/lib/firmware /usr/lib/firmware; do
    FILE="${DIR}/${FIRMWARE}"
    for EXT in "" .zst .xz; do
        [ -e "${FILE}${EXT}" ] || continue
        echo 1 > "${REQUEST}/loading"
        case "${EXT}" in
        .zst) zstd -dc "${FILE}${EXT}" > "${REQUEST}/data" ;;
        .xz) xz -dc "${FILE}${EXT}" > "${REQUEST}/data" ;;
        *) cat "${FILE}" > "${REQUEST}/data" ;;
        esac
        echo 0 > "${REQUEST}/loading"
        exit 0
    done
done

echo -1 > "${REQUEST}/loading"
