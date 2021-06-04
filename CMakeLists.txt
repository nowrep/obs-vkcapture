cmake_minimum_required(VERSION 3.10)

project(obs-vkcapture VERSION 0.5)

include(GNUInstallDirs)
find_package(Vulkan REQUIRED)
find_package(X11)

if (NOT TARGET libobs)
    find_package(LibObs REQUIRED)
endif()

if (VULKAN_FOUND AND NOT TARGET Vulkan::Vulkan)
    add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        IMPORTED_LOCATION "${VULKAN_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${VULKAN_INCLUDE_DIRS}")
endif()

if (X11_xcb_FOUND AND X11_xcb_xfixes_FOUND)
    set(HAVE_X11_XCB TRUE)
endif()

option(BUILD_PLUGIN "Build OBS plugin" ON)

if (${CMAKE_C_FLAGS} MATCHES "-m32")
    set(LAYER_SUFFIX "_32")
else()
    set(LAYER_SUFFIX "_64")
endif()

if (BUILD_PLUGIN)
    set(PLUGIN_SOURCES src/vkcapture.c)
    if (HAVE_X11_XCB)
        set(PLUGIN_SOURCES ${PLUGIN_SOURCES} src/xcursor-xcb.c)
    endif()
    add_library(linux-vkcapture MODULE ${PLUGIN_SOURCES})
    target_link_libraries(linux-vkcapture libobs obs-frontend-api ${X11_xcb_LIB} ${X11_xcb_xfixes_LIB})
    set_target_properties(linux-vkcapture PROPERTIES PREFIX "")
    target_include_directories(linux-vkcapture PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)

    if (COMMAND install_obs_plugin_with_data)
        install_obs_plugin_with_data(linux-vkcapture data)
    else()
        install(TARGETS linux-vkcapture
            LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}/obs-plugins")
        file(GLOB locale_files data/locale/*.ini)
        install(FILES ${locale_files}
            DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/obs/obs-plugins/linux-vkcapture/locale")
    endif()
endif()

set(LAYER_SOURCES src/vklayer.c src/capture.c)
add_library(VkLayer_obs_vkcapture MODULE ${LAYER_SOURCES})
target_link_libraries(VkLayer_obs_vkcapture Vulkan::Vulkan)
set_target_properties(VkLayer_obs_vkcapture PROPERTIES C_STANDARD 11)
target_include_directories(VkLayer_obs_vkcapture PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)

set(GL_SOURCES src/dlsym.c src/elfhacks.c src/glinject.c src/glad/glad.c src/capture.c)
add_library(obs_glcapture MODULE ${GL_SOURCES})
set_target_properties(obs_glcapture PROPERTIES LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/glinject.version")
target_include_directories(obs_glcapture PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)

configure_file(plugin-macros.h.in ${CMAKE_CURRENT_BINARY_DIR}/plugin-macros.h @ONLY)
configure_file(src/obs_vkcapture.json.in ${CMAKE_CURRENT_BINARY_DIR}/obs_vkcapture${LAYER_SUFFIX}.json @ONLY)

install(TARGETS VkLayer_obs_vkcapture
    LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}")

install(TARGETS obs_glcapture
    LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}")

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/obs_vkcapture${LAYER_SUFFIX}.json
    DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/vulkan/implicit_layer.d")

install(PROGRAMS src/obs-vkcapture
    DESTINATION "${CMAKE_INSTALL_FULL_BINDIR}")

install(PROGRAMS src/obs-glcapture
    DESTINATION "${CMAKE_INSTALL_FULL_BINDIR}")
