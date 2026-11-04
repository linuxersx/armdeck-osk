cmake_minimum_required(VERSION 3.16)
project(armdeck-osk VERSION 1.0.0 LANGUAGES C)

set(ARMDECK_BUILD_NUMBER "" CACHE STRING "Build number suffix")
if(NOT ARMDECK_BUILD_NUMBER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_NUMBER")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_NUMBER" ARMDECK_BUILD_NUMBER)
  string(STRIP "${ARMDECK_BUILD_NUMBER}" ARMDECK_BUILD_NUMBER)
endif()
if(ARMDECK_BUILD_NUMBER)
  set(ARMDECK_VERSION "${PROJECT_VERSION}.${ARMDECK_BUILD_NUMBER}")
else()
  set(ARMDECK_VERSION "${PROJECT_VERSION}")
endif()
message(STATUS "ArmDeck OSK version: ${ARMDECK_VERSION}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(PANGO REQUIRED pango pangocairo)
pkg_check_modules(WAYLAND REQUIRED wayland-client)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/protocols")
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${GEN_DIR}")

function(generate_protocol xml name)
  add_custom_command(
    OUTPUT "${GEN_DIR}/${name}-client-protocol.h"
           "${GEN_DIR}/${name}-protocol.c"
    COMMAND ${WAYLAND_SCANNER} client-header "${xml}" "${GEN_DIR}/${name}-client-protocol.h"
    COMMAND ${WAYLAND_SCANNER} private-code "${xml}" "${GEN_DIR}/${name}-protocol.c"
    DEPENDS "${xml}"
    VERBATIM)
endfunction()

generate_protocol("${PROTO_DIR}/wlr-layer-shell-unstable-v1.xml" "wlr-layer-shell-unstable-v1")
generate_protocol("/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml" "xdg-shell")
set(INPUT_METHOD_XML "${PROTO_DIR}/input-method-unstable-v2.xml")
if(EXISTS "${INPUT_METHOD_XML}")
  generate_protocol("${INPUT_METHOD_XML}" "input-method-unstable-v2")
  set(IM_SRC "${GEN_DIR}/input-method-unstable-v2-protocol.c")
  set(IM_HDR "${GEN_DIR}/input-method-unstable-v2-client-protocol.h")
else()
  message(FATAL_ERROR "missing input-method-unstable-v2.xml")
endif()

add_executable(armdeck-osk
  src/main.c
  src/osk.c
  src/rime_engine.c
  src/config.c
  src/theme.c
  "${GEN_DIR}/wlr-layer-shell-unstable-v1-protocol.c"
  "${GEN_DIR}/xdg-shell-protocol.c"
  "${IM_SRC}"
)

target_include_directories(armdeck-osk PRIVATE
  src
  "${GEN_DIR}"
  ${CAIRO_INCLUDE_DIRS}
  ${PANGO_INCLUDE_DIRS}
  ${WAYLAND_INCLUDE_DIRS}
)
target_link_libraries(armdeck-osk PRIVATE
  ${CAIRO_LIBRARIES}
  ${PANGO_LIBRARIES}
  ${WAYLAND_LIBRARIES}
  m pthread ${CMAKE_DL_LIBS}
)
target_compile_options(armdeck-osk PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(armdeck-oskd src/supervisor.c)
target_compile_options(armdeck-oskd PRIVATE -Wall -Wextra)
install(TARGETS armdeck-oskd RUNTIME DESTINATION bin)

add_executable(armdeck-osk-input src/input_daemon.c)
target_compile_options(armdeck-osk-input PRIVATE -Wall -Wextra)
install(TARGETS armdeck-osk armdeck-osk-input RUNTIME DESTINATION bin)
install(FILES systemd/armdeck-oskd.service systemd/armdeck-osk-input.service
        DESTINATION lib/systemd/user)
install(FILES udev/99-armdeck-osk-input.rules DESTINATION lib/udev/rules.d)
install(FILES sway/armdeck-osk.conf DESTINATION share/armdeck-osk)
install(FILES LICENSE THIRD_PARTY.md DESTINATION share/licenses/armdeck-osk)

# ── LVGL settings application ───────────────────────────────────────────────
option(ARMDECK_BUILD_GUI "Build the LVGL settings application" ON)
if(ARMDECK_BUILD_GUI)
  find_package(Threads REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  pkg_check_modules(PNG REQUIRED libpng)
  pkg_check_modules(FONTCONFIG REQUIRED fontconfig)

  file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl/src/*.c")
  list(FILTER LVGL_SOURCES EXCLUDE REGEX "[ ]2\\.c$")
  add_library(armdeck_lvgl STATIC ${LVGL_SOURCES})
  target_include_directories(armdeck_lvgl PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${PNG_INCLUDE_DIRS})
  target_link_libraries(armdeck_lvgl PUBLIC ${PNG_LIBRARIES} m)
  target_compile_definitions(armdeck_lvgl PUBLIC
    LV_CONF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h")
  target_compile_options(armdeck_lvgl PRIVATE -w)

  add_executable(armdeck-osk-settings
    gui/main.c
    gui/ui.c
    gui/backend.c
    src/config.c
    src/theme.c)
  target_include_directories(armdeck-osk-settings PRIVATE
    gui src
    ${SDL2_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS})
  target_link_libraries(armdeck-osk-settings PRIVATE
    armdeck_lvgl
    ${SDL2_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    m Threads::Threads z)
  target_compile_options(armdeck-osk-settings PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_compile_definitions(armdeck-osk-settings PRIVATE
    ARMDECK_OSK_VERSION="${ARMDECK_VERSION}")
  install(TARGETS armdeck-osk-settings RUNTIME DESTINATION bin)
  install(FILES gui/armdeck-osk-settings.desktop DESTINATION share/applications)
  install(FILES gui/armdeck-osk-settings.svg
          DESTINATION share/icons/hicolor/scalable/apps)
  install(FILES gui/armdeck-osk-settings.png DESTINATION share/pixmaps)
endif()
