cmake_minimum_required(VERSION 3.16)

# v3.9.10: protected defaults live outside the sdkconfig.* wildcard namespace.
# ESP-IDF officially supports SDKCONFIG_DEFAULTS as a list of default config files.
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/config/b14_sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(b14v310)

# Fail early if the protected defaults were not merged.
if(NOT CONFIG_COMPILER_CXX_EXCEPTIONS)
    message(FATAL_ERROR "B14 v3.9.10 requires CONFIG_COMPILER_CXX_EXCEPTIONS=y from config/b14_sdkconfig.defaults")
endif()

if(NOT CONFIG_ESPTOOLPY_FLASHSIZE_8MB)
    message(FATAL_ERROR "B14 v3.9.10 requires CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y from config/b14_sdkconfig.defaults")
endif()
