#!/bin/sh
# Rebuild the native executable (MSVC) quietly; print only errors.
powershell -NoProfile -Command "& cmd /c '$(cygpath -w "$(dirname "$0")")\build_native.bat' 2>&1 | Select-String -Pattern 'error|FAILED' | Select-Object -First 30"
test -f "$(dirname "$0")/build/native/IRC.exe"
