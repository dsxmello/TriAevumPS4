# Host-configure stub (tools/product): only the target the closure needs.
set(nlohmann_json_FOUND TRUE)
set(nlohmann_json_VERSION 99.0.0)
if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
endif()
