cmake_minimum_required(VERSION 3.10)

project(obs-vkcapture VERSION 0.3)

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

    install(TARGETS linux-vkcapture
        LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}/obs-plugins")

    file(GLOB locale_files data/locale/*.ini)
    install(FILES ${locale_files}
        DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/obs/obs-plugins/linux-vkcapture/locale")
endif()

set(LAYER_SOURCES src/vklayer.c src/capture.c)
add_library(VkLayer_obs_vkcapture MODULE ${LAYER_SOURCES})
target_link_libraries(VkLayer_obs_vkcapture Vulkan::Vulkan)

set(GL_SOURCES src/glinject.c src/glad/glad.c src/capture.c)
add_library(obs_glcapture MODULE ${GL_SOURCES})

configure_file(plugin-macros.h.in ${CMAKE_BINARY_DIR}/plugin-macros.h @ONLY)
configure_file(src/obs_vkcapture.json.in ${CMAKE_BINARY_DIR}/obs_vkcapture${LAYER_SUFFIX}.json @ONLY)
configure_file(src/obs-glcapture.in ${CMAKE_BINARY_DIR}/obs-glcapture @ONLY)

install(TARGETS VkLayer_obs_vkcapture
    LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}")

install(TARGETS obs_glcapture
    LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}")

install(FILES ${CMAKE_BINARY_DIR}/obs_vkcapture${LAYER_SUFFIX}.json
    DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/vulkan/implicit_layer.d")

install(FILES ${CMAKE_BINARY_DIR}/obs-vkcapture
    DESTINATION "${CMAKE_INSTALL_FULL_BINDIR}")

install(FILES ${CMAKE_BINARY_DIR}/obs-glcapture
    DESTINATION "${CMAKE_INSTALL_FULL_BINDIR}")
