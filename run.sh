#!/usr/bin/env bash
set -exo pipefail
ndk-build
abi=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')
adb push libs/$abi/minitouch /data/local/tmp/
adb shell /data/local/tmp/minitouch "$@"
