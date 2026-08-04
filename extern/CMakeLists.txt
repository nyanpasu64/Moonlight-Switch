add_subdirectory(borealis/library)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

set(BUILD_SHARED_LIBS OFF)

if (USE_MBEDTLS_CRYPTO)
    set(USE_MBEDTLS ON)
endif ()

option(TRACY_ENABLE "" ON)
option(TRACY_NO_FRAME_IMAGE "" ON)
option(TRACY_DELAYED_INIT "" ON)
option(TRACY_MANUAL_LIFETIME "" ON)
add_subdirectory(tracy)  # target: TracyClient or alias Tracy::TracyClient

add_subdirectory(moonlight-common-c)

if (NOT MULTICAST_DISABLED)
    set(MDNS_BUILD_EXAMPLE OFF)
    add_subdirectory(mdns)
endif ()
