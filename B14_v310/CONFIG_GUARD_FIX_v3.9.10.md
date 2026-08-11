# v3.9.10 CONFIG-GUARD FIX

## Root cause
The previous cleanup used `Remove-Item sdkconfig.*`. In PowerShell, that wildcard also matches `sdkconfig.defaults`, so the committed defaults file was deleted before CMake started.

## Fix
- Protected source config moved to `config/b14_sdkconfig.defaults`.
- Root `CMakeLists.txt` sets `SDKCONFIG_DEFAULTS` before including ESP-IDF's project.cmake.
- `BUILD_CLEAN.ps1` deletes only generated sdkconfig files by exact names.
- C++ exceptions are enabled in the protected config.
- Build target uses a conservative 8 MB flash layout; current partitions fit within 8 MB.
- Short project/workspace paths remain enabled.
