cmake_minimum_required(VERSION 2.8.7)

project(imlibpurpleservice CXX)

include(webOS/webOS)
webos_modules_init(1 0 0 QUALIFIER RC2)
webos_component(3 0 5)

include(FindPkgConfig)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
include_directories(${GLIB2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GLIB2_CFLAGS})

pkg_check_modules(GIO2 REQUIRED gio-2.0)
include_directories(${GIO2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GIO2_CFLAGS})

pkg_check_modules(GIO-UNIX REQUIRED gio-unix-2.0)
include_directories(${GIO-UNIX_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GIO-UNIX_CFLAGS_OTHER})

pkg_check_modules(LUNASERVICE2 REQUIRED luna-service2)
include_directories(${LUNASERVICE2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${LUNASERVICE2_CFLAGS})

pkg_check_modules(MOJODB REQUIRED mojodb)
include_directories(${MOJODB_INCUDE_DIRS})
webos_add_compiler_flags(ALL ${MOJODB_CFLAGS})

pkg_check_modules(PURPLE REQUIRED purple)
include_directories(${PURPLE_INCUDE_DIRS})
webos_add_compiler_flags(ALL ${PURPLE_CFLAGS})

webos_add_compiler_flags(ALL "-DMOJ_LINUX -fno-rtti -std=c++17")

# Legacy db8 (e.g. webOS 3.0.5) has no com.palm.config.libpurple:1 kind, so the
# modern per-account config-management path (config lookup on account enable +
# config put/del on create/delete) would fail there and block login. Enable this
# to bypass it and fall back to the classic pre-config-management behaviour.
option(IMLIBPURPLE_LEGACY_DB8 "Bypass modern config-management db8 kind (legacy webOS)" OFF)
if(IMLIBPURPLE_LEGACY_DB8)
    webos_add_compiler_flags(ALL "-DIMLIBPURPLE_LEGACY_DB8")
endif()

file(GLOB SOURCES
    src/OnEnabledHandler.cpp
    src/IMLoginState.cpp
    src/LibpurpleAdapter.cpp
    src/BuddyStatusHandler.cpp
    src/IMServiceHandler.cpp
    src/SendOneMessageHandler.cpp
    src/DisplayController.cpp
    src/OutgoingIMHandler.cpp
    src/BuddyListConsolidator.cpp
    src/IMServiceApp.cpp
    src/ConnectionStateHandler.cpp
    src/IMMessage.cpp
    src/sanitize.cpp
    src/entities.cpp
    src/SendOneCommandHandler.cpp
    src/OutgoingIMCommandHandler.cpp
    src/IncomingIMHandler.cpp
    src/Util.cpp)

include_directories(${CMAKE_SOURCE_DIR}/inc)

add_executable(imlibpurpleservice ${SOURCES})
target_link_libraries(imlibpurpleservice
    ${GLIB2_LDFLAGS} ${LUNASERVICE2_LDFLAGS}
    ${GIO2_LDFLAGS} ${GIO-UNIX_LDFLAGS}
    ${MOJODB_LDFLAGS} ${PURPLE_LDFLAGS} tidy rt pthread)

webos_build_daemon()
webos_build_system_bus_files()
