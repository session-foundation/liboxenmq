
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  foreach(lang C CXX)
    if(NOT DEFINED CMAKE_${lang}_COMPILER_LAUNCHER AND NOT CMAKE_${lang}_COMPILER MATCHES ".*/ccache")
      message(STATUS "Enabling ccache for ${lang}")
      set(CMAKE_${lang}_COMPILER_LAUNCHER ${CCACHE_PROGRAM} CACHE STRING "")
    endif()
  endforeach()
endif()

cmake_minimum_required(VERSION 3.10...3.31)

# Has to be set before `project()`, and ignored on non-macos:
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.12 CACHE STRING "macOS deployment target (Apple clang only)")

project(liboxenmq
    VERSION 1.3.2
    LANGUAGES CXX C)

include(GNUInstallDirs)
include(cmake/libatomic.cmake)

message(STATUS "oxenmq v${PROJECT_VERSION}")

set(OXENMQ_LIBVERSION 0)


if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(oxenmq_IS_TOPLEVEL_PROJECT TRUE)
else()
    set(oxenmq_IS_TOPLEVEL_PROJECT FALSE)
endif()


option(BUILD_SHARED_LIBS "Build shared libraries instead of static ones" ON)
set(oxenmq_INSTALL_DEFAULT OFF)
if(BUILD_SHARED_LIBS OR oxenmq_IS_TOPLEVEL_PROJECT)
    set(oxenmq_INSTALL_DEFAULT ON)
endif()
set(oxenmq_EPOLL_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    set(oxenmq_EPOLL_DEFAULT ON)
endif()

option(OXENMQ_BUILD_TESTS "Building and perform oxenmq tests" ${oxenmq_IS_TOPLEVEL_PROJECT})
option(OXENMQ_INSTALL "Add oxenmq libraries and headers to cmake install target; defaults to ON if BUILD_SHARED_LIBS is enabled or we are the top-level project; OFF for a static subdirectory build" ${oxenmq_INSTALL_DEFAULT})
option(OXENMQ_INSTALL_CPPZMQ "Install cppzmq header with oxenmq/ headers (requires OXENMQ_INSTALL)" ON)
option(OXENMQ_USE_EPOLL "Build with epoll support for socket polling (requires Linux)" ${oxenmq_EPOLL_DEFAULT})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# session-deps provides libzmq and libsodium: system libraries when acceptable, static builds
# otherwise (BUILD_STATIC_DEPS, or per library BUILD_STATIC_libzmq / BUILD_STATIC_libsodium).  Its
# static builds use LTO by default, which a link that does not itself use LTO cannot consume under
# clang; nothing here enables LTO, so it is off unless a parent project has already decided.
set(SESSIONDEPS_LTO OFF CACHE BOOL "Use LTO for static dependency builds, where supported")
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/session-deps/Deps.cmake)
check_submodule(cmake/session-deps)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

configure_file(oxenmq/version.h.in oxenmq/version.h @ONLY)


add_library(oxenmq
    oxenmq/address.cpp
    oxenmq/auth.cpp
    oxenmq/connections.cpp
    oxenmq/jobs.cpp
    oxenmq/oxenmq.cpp
    oxenmq/proxy.cpp
    oxenmq/worker.cpp
)
set_target_properties(oxenmq PROPERTIES SOVERSION ${OXENMQ_LIBVERSION})
if(OXENMQ_USE_EPOLL)
    target_compile_definitions(oxenmq PRIVATE OXENMQ_USE_EPOLL)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(oxenmq PRIVATE Threads::Threads)


if(TARGET oxen::logging)
    add_library(_oxenmq_external_logging INTERFACE IMPORTED)
    target_link_libraries(_oxenmq_external_logging INTERFACE oxen::logging)
    target_link_libraries(oxenmq PRIVATE _oxenmq_external_logging)
    message(STATUS "using pre-existing oxen::logging target")
else()
    if(APPLE)
        # LTO is usually broken on macOS, just like most things under the surface.
        set(USE_LTO OFF CACHE INTERNAL "")
    endif()
    # oxen-logging resolves oxen::logging itself: a system liboxen-logging together with suitable
    # fmt and spdlog targets, or else the submodule.  OXEN_LOGGING_FORCE_SUBMODULES (implied by
    # BUILD_STATIC_DEPS) skips the system library and takes the bundled fmt and spdlog too.
    include(${CMAKE_CURRENT_SOURCE_DIR}/oxen-logging/cmake/load.cmake)
    target_link_libraries(oxenmq PUBLIC oxen::logging)
endif()


if(TARGET oxenc::oxenc)
    add_library(_oxenmq_external_oxenc INTERFACE IMPORTED)
    target_link_libraries(_oxenmq_external_oxenc INTERFACE oxenc::oxenc)
    target_link_libraries(oxenmq PUBLIC _oxenmq_external_oxenc)
    message(STATUS "using pre-existing oxenc::oxenc target")
elseif(BUILD_SHARED_LIBS)
    include(FindPkgConfig)
    pkg_check_modules(oxenc liboxenc>=1.4.0 IMPORTED_TARGET)

    if(oxenc_FOUND)
        # Work around cmake bug 22180 (PkgConfig::tgt not set if no flags needed)
        if(TARGET PkgConfig::oxenc OR CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
            target_link_libraries(oxenmq PUBLIC PkgConfig::oxenc)
        endif()
    else()
        add_subdirectory(oxen-encoding)
        target_link_libraries(oxenmq PUBLIC oxenc::oxenc)
    endif()
else()
    add_subdirectory(oxen-encoding)
    target_link_libraries(oxenmq PUBLIC oxenc::oxenc)
endif()

# The static libzmq is worth asking for (BUILD_STATIC_libzmq) even where a system one exists: every
# release through 4.3.5 can abort the process when a connection dies while its receive pipe is full,
# and session-deps' copy carries the fix (see deps/libzmq.cmake there).  A parent project that has
# already set up a `libzmq` target of its own keeps it.
if(TARGET libzmq)
    target_link_libraries(oxenmq PUBLIC libzmq)
else()
    if(NOT BUILD_SHARED_LIBS)
        # A static oxenmq has always brought its own libzmq rather than gambling on a system static
        # archive, whose own dependencies are rarely all available statically.  A default, not
        # forced: -DBUILD_STATIC_libzmq=OFF still picks a system libzmq known to be good.
        option(BUILD_STATIC_libzmq "Build against session-deps' static libzmq rather than a system one" ON)
    endif()
    session_dep(libzmq 4.3)
    if(TARGET sessiondep_ext_libzmq)
        set(oxenmq_bundled_libzmq ON)
    endif()
    if(BUILD_SHARED_LIBS AND oxenmq_bundled_libzmq)
        # A static libzmq inside the shared library is an implementation detail: nothing links it
        # from outside, and its zmq_* symbols are kept out of the exports so that a system libzmq
        # loaded into the same process can never be picked in its place at symbol resolution and
        # quietly reintroduce the bug the static copy fixes.  Consumers still need zmq.h for
        # zmq.hpp, so the headers stay on the interface.
        set(oxenmq_private_libzmq ON)
        target_link_libraries(oxenmq PRIVATE sessiondep::libzmq)
        target_include_directories(oxenmq PUBLIC
            $<BUILD_INTERFACE:$<TARGET_PROPERTY:sessiondep::libzmq,INTERFACE_INCLUDE_DIRECTORIES>>)
        if(NOT APPLE)
            target_link_options(oxenmq PRIVATE "LINKER:--exclude-libs,libzmq.a")
        endif()
    else()
        target_link_libraries(oxenmq PUBLIC sessiondep::libzmq)
    endif()
endif()

target_include_directories(oxenmq
    PUBLIC
        $<INSTALL_INTERFACE:>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cppzmq>
)

target_compile_options(oxenmq PRIVATE -Wall -Wextra)

option(WARNINGS_AS_ERRORS "treat all warnings as errors" OFF)
if(WARNINGS_AS_ERRORS)
    target_compile_options(oxenmq PRIVATE -Werror)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13.0.0)
    message(STATUS "Disabling -Werror for restrict-qualified parameters")
    target_compile_options(oxenmq PRIVATE -Wno-error=restrict)
endif()

# libsodium likewise; a parent's `sodium` target takes precedence.
if(TARGET sodium)
    target_link_libraries(oxenmq PUBLIC sodium)
else()
    session_dep(libsodium 1.0.17)
    target_link_libraries(oxenmq PUBLIC sessiondep::libsodium)
endif()

add_library(oxenmq::oxenmq ALIAS oxenmq)

# oxenmq.h includes zmq.hpp, which includes <zmq.h>, so consumers need libzmq's header even though
# nothing else of libzmq reaches them.  With a bundled libzmq that header is installed beside
# zmq.hpp, and its directory goes on the pkg-config search path because an angle-bracket include
# does not look beside the including file; there is then no libzmq.pc for Requires.private either.
set(OXENMQ_PC_REQUIRES_PRIVATE "libzmq libsodium")
set(OXENMQ_PC_CFLAGS "-I\${includedir}")
if(oxenmq_bundled_libzmq)
    set(OXENMQ_PC_REQUIRES_PRIVATE "libsodium")
    set(OXENMQ_PC_CFLAGS "-I\${includedir} -I\${includedir}/oxenmq")
endif()
configure_file(liboxenmq.pc.in liboxenmq.pc @ONLY)

if(OXENMQ_INSTALL)
    install(
        TARGETS oxenmq
        EXPORT oxenmqConfig
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )

    install(
        FILES oxenmq/address.h
        oxenmq/auth.h
        oxenmq/batch.h
        oxenmq/connections.h
        oxenmq/fmt.h
        oxenmq/message.h
        oxenmq/oxenmq.h
        oxenmq/pubsub.h
        ${CMAKE_CURRENT_BINARY_DIR}/oxenmq/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/oxenmq
        )

    if(OXENMQ_INSTALL_CPPZMQ)
        install(
            FILES cppzmq/zmq.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/oxenmq
            )
        if(oxenmq_bundled_libzmq)
            install(
                FILES ${SESSIONDEPS_DESTDIR}/include/zmq.h
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/oxenmq
                )
        endif()
    endif()


    install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/liboxenmq.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )

endif()

if(OXENMQ_BUILD_TESTS)
    add_subdirectory(tests)
endif()
