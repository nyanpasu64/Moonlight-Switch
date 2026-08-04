add_subdirectory(borealis/library)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

set(BUILD_SHARED_LIBS OFF)

if (USE_MBEDTLS_CRYPTO)
    set(USE_MBEDTLS ON)
endif ()

# add_subdirectory(extern/tracy)  # target: TracyClient or alias Tracy::TracyClient
add_library(TracyClient INTERFACE)
# for some reason __SWITCH__ is only defined on executables and not libraries, and the tracy fork depends on it. just stuff its source into ${PROJECT_NAME}.
# cmake/Platform/NintendoSwitch.cmake
# 15:set(NX_COMMON_FLAGS  "-ffunction-sections -fdata-sections -D__SWITCH__")
# 25:             set(CMAKE_${lang}_FLAGS_INIT "${${platform}_ARCH_SETTINGS} ${${platform}_COMMON_FLAGS}")

target_include_directories(TracyClient SYSTEM INTERFACE tracy/public)
target_compile_definitions(TracyClient INTERFACE TRACY_ENABLE=1 TRACY_PLATFORM_HEADER="\"tracy-switch.hpp\"")

add_subdirectory(moonlight-common-c)

if (NOT MULTICAST_DISABLED)
    set(MDNS_BUILD_EXAMPLE OFF)
    add_subdirectory(mdns)
endif ()
