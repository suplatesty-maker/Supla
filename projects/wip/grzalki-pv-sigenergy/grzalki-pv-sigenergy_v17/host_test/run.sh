#!/usr/bin/env bash
# Runs the v17 control Engine on the host. No ESP32, no hardware.
# Only Arduino/FreeRTOS/1-Wire/Supla-HTML are stubbed in ./stubs;
# controller.h, control_logic.h and settings.h are the shipping files.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/build"
mkdir -p "$out"
CXX="${CXX:-g++}"
FLAGS=(-std=c++17 -O1 -Wall -Wextra -I "$here/stubs" -I "$(dirname "$here")")
status=0
for src in "$here"/scenarios*.cpp; do
  name="$(basename "$src" .cpp)"
  echo "== $name"
  "$CXX" "${FLAGS[@]}" "$src" -o "$out/$name"
  "$out/$name" || status=1
done
exit $status
