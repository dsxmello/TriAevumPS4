# Host-configure stub (tools/product): only the target the closure needs.
set(spdlog_FOUND TRUE)
set(spdlog_VERSION 99.0.0)
if(NOT TARGET spdlog::spdlog)
    add_library(spdlog::spdlog INTERFACE IMPORTED)
endif()
