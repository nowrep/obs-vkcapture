cmake_minimum_required(VERSION 3.10)

project(obs-vkcapture VERSION 1.3.1)

include(GNUInstallDirs)
find_package(Vulkan REQUIRED)
set(OpenGL_GL_PREFERENCE LEGACY)
find_package(OpenGL REQUIRED)
find_package(PkgConfig)
pkg_check_modules(EGL egl IMPORTED_TARGET)
pkg_check_modules(X11 x11 IMPORTED_TARGET)
pkg_check_modules(XCB xcb IMPORTED_TARGET)
pkg_check_modules(XCB_XFIXES xcb-xfixes IMPORTED_TARGET)
pkg_check_modules(WAYLAND_CLIENT wayland-client IMPORTED_TARGET)
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)

if (VULKAN_FOUND AND NOT TARGET Vulkan::Vulkan)
    add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        IMPORTED_LOCATION "${VULKAN_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${VULKAN_INCLUDE_DIRS}")
endif()
if (NOT Vulkan_VERSION)
    set(Vulkan_VERSION "1.3.221")
endif()

if (X11_FOUND)
    set(HAVE_X11_XLIB TRUE)
endif()
if (XCB_FOUND AND XCB_XFIXES_FOUND)
    set(HAVE_X11_XCB TRUE)
endif()
if (WAYLAND_CLIENT_FOUND AND WAYLAND_SCANNER)
    set(HAVE_WAYLAND TRUE)
endif()

option(BUILD_PLUGIN "Build OBS plugin" ON)

if (${CMAKE_SIZEOF_VOID_P} EQUAL 4)
    set(LAYER_SUFFIX "_32")
else()
    set(LAYER_SUFFIX "_64")
endif()

set(CMAKE_C_STANDARD 11)

if (BUILD_PLUGIN)
    if (NOT TARGET libobs)
        find_package(LibObs QUIET)
    endif()
    if (TARGET libobs)
        set(PLUGIN_LIBS libobs)
    elseif(TARGET OBS::libobs)
        set(PLUGIN_LIBS OBS::libobs)
    else()
        pkg_check_modules(LIBOBS libobs IMPORTED_TARGET REQUIRED)
        set(PLUGIN_LIBS PkgConfig::LIBOBS)
        if (HAVE_WAYLAND)
            add_definitions(-DENABLE_WAYLAND=1)
        endif()
    endif()

    set(PLUGIN_SOURCES src/vkcapture.c)
    set(PLUGIN_LIBS ${PLUGIN_LIBS} obs-frontend-api PkgConfig::EGL)
    if (HAVE_X11_XCB)
        set(PLUGIN_SOURCES ${PLUGIN_SOURCES} src/xcursor-xcb.c)
        set(PLUGIN_LIBS ${PLUGIN_LIBS} PkgConfig::XCB PkgConfig::XCB_XFIXES)
    endif()
    if (HAVE_WAYLAND)
        set(screencopy_protocol "${CMAKE_CURRENT_SOURCE_DIR}/src/ext-screencopy-v1.xml")
        set(screencopy_header "${CMAKE_CURRENT_BINARY_DIR}/screencopy_unstable_v1.h")
        set(screencopy_source "${CMAKE_CURRENT_BINARY_DIR}/screencopy_unstable_v1.c")
        add_custom_command(OUTPUT ${screencopy_header}
            COMMAND ${WAYLAND_SCANNER} client-header ${screencopy_protocol} ${screencopy_header}
            DEPENDS ${screencopy_protocol})
        add_custom_command(OUTPUT ${screencopy_source}
            COMMAND ${WAYLAND_SCANNER} private-code ${screencopy_protocol} ${screencopy_source}
            DEPENDS ${screencopy_protocol})
        set(PLUGIN_SOURCES ${PLUGIN_SOURCES} src/wlcursor.c ${screencopy_header} ${screencopy_source})
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
configure_file(src/obs-gamecapture.in ${CMAKE_CURRENT_BINARY_DIR}/obs-gamecapture @ONLY)

install(TARGETS VkLayer_obs_vkcapture
    LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}")

install(TARGETS obs_glcapture
    LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}")

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/obs_vkcapture${LAYER_SUFFIX}.json
    DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/vulkan/implicit_layer.d")

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/obs-gamecapture
    src/obs-vkcapture src/obs-glcapture
    DESTINATION "${CMAKE_INSTALL_FULL_BINDIR}")
