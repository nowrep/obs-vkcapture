cmake_minimum_required(VERSION 3.10)

project(linux-vkcapture VERSION 1.0.0)

set(CMAKE_PREFIX_PATH "${QTDIR}")
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

find_package(LibObs REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Core Widgets)
find_package(Vulkan REQUIRED)

configure_file(
    plugin-macros.h.in
    ${CMAKE_BINARY_DIR}/plugin-macros.h
)

set(PLUGIN_SOURCES
    src/vkcapture.c)

add_library(${CMAKE_PROJECT_NAME} MODULE ${PLUGIN_SOURCES})

include_directories(${CMAKE_BINARY_DIR})

target_link_libraries(${CMAKE_PROJECT_NAME}
    libobs
    Qt5::Core
    Qt5::Widgets
)

set(LAYER_SOURCES
    src/vklayer.c
)
add_library(VkLayer_obs_vkcapture MODULE ${LAYER_SOURCES})
target_link_libraries(VkLayer_obs_vkcapture Vulkan::Vulkan)
configure_file(src/obs_vkcapture.json.in ${CMAKE_BINARY_DIR}/obs_vkcapture.json)

include(GNUInstallDirs)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
target_link_libraries(${CMAKE_PROJECT_NAME} obs-frontend-api)

file(GLOB locale_files data/locale/*.ini)

install(TARGETS ${CMAKE_PROJECT_NAME}
    LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}/obs-plugins")

install(FILES ${CMAKE_BINARY_DIR}/obs_vkcapture.json
    DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/vulkan/implicit_layer.d")

install(FILES ${locale_files}
    DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/obs/obs-plugins/${CMAKE_PROJECT_NAME}/locale")
