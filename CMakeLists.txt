cmake_minimum_required(VERSION 3.10)

project(obs-vkcapture VERSION 1.1.0)

include(GNUInstallDirs)
find_package(Vulkan REQUIRED)
set(OpenGL_GL_PREFERENCE LEGACY)
find_package(OpenGL REQUIRED)
find_package(PkgConfig)
pkg_check_modules(X11 x11 IMPORTED_TARGET)
pkg_check_modules(XCB xcb IMPORTED_TARGET)
pkg_check_modules(XCB_XFIXES xcb-xfixes IMPORTED_TARGET)
pkg_check_modules(WAYLAND_CLIENT wayland-client IMPORTED_TARGET)

if (NOT TARGET libobs)
    find_package(LibObs REQUIRED)
endif()

if (VULKAN_FOUND AND NOT TARGET Vulkan::Vulkan)
    add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        IMPORTED_LOCATION "${VULKAN_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${VULKAN_INCLUDE_DIRS}")
endif()

if (X11_FOUND)
    set(HAVE_X11_XLIB TRUE)
endif()
if (XCB_FOUND AND XCB_XFIXES_FOUND)
    set(HAVE_X11_XCB TRUE)
endif()
if (WAYLAND_CLIENT_FOUND)
    set(HAVE_WAYLAND TRUE)
endif()

option(BUILD_PLUGIN "Build OBS plugin" ON)

if (${CMAKE_C_FLAGS} MATCHES "-m32")
    set(LAYER_SUFFIX "_32")
else()
    set(LAYER_SUFFIX "_64")
endif()

set(CMAKE_C_STANDARD 11)

if (BUILD_PLUGIN)
    set(PLUGIN_SOURCES src/vkcapture.c)
    set(PLUGIN_LIBS libobs obs-frontend-api)
    if (HAVE_X11_XCB)
        set(PLUGIN_SOURCES ${PLUGIN_SOURCES} src/xcursor-xcb.c)
        set(PLUGIN_LIBS ${PLUGIN_LIBS} PkgConfig::XCB PkgConfig::XCB_XFIXES)
    endif()
    if (HAVE_WAYLAND)
        set(PLUGIN_SOURCES ${PLUGIN_SOURCES} src/wlcursor.c src/screencopy_unstable_v1.c)
        set(PLUGIN_LIBS ${PLUGIN_LIBS} PkgConfig::WAYLAND_CLIENT)
    endif()
    add_library(linux-vkcapture MODULE ${PLUGIN_SOURCES})
    target_link_libraries(linux-vkcapture ${PLUGIN_LIBS})
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
set_target_properties(VkLayer_obs_vkcapture PROPERTIES LINK_FLAGS "-Wl,--version-script=\"${CMAKE_CURRENT_SOURCE_DIR}/src/vklayer.version\"")
target_link_libraries(VkLayer_obs_vkcapture Vulkan::Vulkan)
target_include_directories(VkLayer_obs_vkcapture PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
if (HAVE_X11_XLIB)
    target_include_directories(VkLayer_obs_vkcapture PRIVATE $<TARGET_PROPERTY:PkgConfig::X11,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if (HAVE_X11_XCB)
    target_include_directories(VkLayer_obs_vkcapture PRIVATE
        $<TARGET_PROPERTY:PkgConfig::XCB,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:PkgConfig::XCB_XFIXES,INTERFACE_INCLUDE_DIRECTORIES>
    )
endif()

set(GL_SOURCES src/dlsym.c src/elfhacks.c src/glinject.c src/capture.c)
add_library(obs_glcapture MODULE ${GL_SOURCES})
set_target_properties(obs_glcapture PROPERTIES LINK_FLAGS "-Wl,--version-script=\"${CMAKE_CURRENT_SOURCE_DIR}/src/glinject.version\"")
target_link_libraries(obs_glcapture ${CMAKE_DL_LIBS} OpenGL::GL)
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
