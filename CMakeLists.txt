cmake_minimum_required(VERSION 3.10)

project(obs-vkcapture VERSION 1.0.0)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

include(GNUInstallDirs)
find_package(LibObs REQUIRED)
find_package(Vulkan REQUIRED)

option(BUILD_PLUGIN "Build OBS plugin" ON)

if (${CMAKE_C_FLAGS} MATCHES "-m32")
    set(LAYER_SUFFIX "_32")
else()
    set(LAYER_SUFFIX "_64")
endif()

if (BUILD_PLUGIN)
    set(PLUGIN_SOURCES src/vkcapture.c)
    add_library(linux-vkcapture MODULE ${PLUGIN_SOURCES})
    target_link_libraries(linux-vkcapture libobs obs-frontend-api)
    set_target_properties(linux-vkcapture PROPERTIES PREFIX "")

    configure_file(plugin-macros.h.in ${CMAKE_BINARY_DIR}/plugin-macros.h)

    install(TARGETS linux-vkcapture
        LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}/obs-plugins")

    file(GLOB locale_files data/locale/*.ini)
    install(FILES ${locale_files}
        DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/obs/obs-plugins/linux-vkcapture/locale")
endif()

set(LAYER_SOURCES src/vklayer.c)
add_library(VkLayer_obs_vkcapture MODULE ${LAYER_SOURCES})
target_link_libraries(VkLayer_obs_vkcapture Vulkan::Vulkan)

configure_file(src/obs_vkcapture.json.in ${CMAKE_BINARY_DIR}/obs_vkcapture${LAYER_SUFFIX}.json)

install(TARGETS VkLayer_obs_vkcapture
    LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}")

install(FILES ${CMAKE_BINARY_DIR}/obs_vkcapture${LAYER_SUFFIX}.json
    DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/vulkan/implicit_layer.d")
