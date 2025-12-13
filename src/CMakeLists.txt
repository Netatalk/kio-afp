set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(kio_afp_sources
    kafp_worker.cpp
    afploginwidget.cpp
)

# Find libafpclient via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAFPCLIENT REQUIRED libafpclient)

# Find the libafpclient library file from the pkg-config location so we can
# link against the full path (portable, supports custom prefixes).
find_library(LIBAFPCLIENT_LIBRARY_PATH
    NAMES afpclient
    PATHS ${LIBAFPCLIENT_LIBRARY_DIRS}
    NO_DEFAULT_PATH
)
if (NOT LIBAFPCLIENT_LIBRARY_PATH)
    message(FATAL_ERROR "Could not locate libafpclient in ${LIBAFPCLIENT_LIBRARY_DIRS}")
endif()

# Install to the standard KIO worker executable location (libexec). Use fallbacks if KDE vars are missing.
set(KIO_WORKER_INSTALL_DIR "${KDE_INSTALL_LIBEXECDIR_KF6}")
if(NOT KIO_WORKER_INSTALL_DIR)
    if(DEFINED KDE_INSTALL_LIBEXECDIR)
        set(KIO_WORKER_INSTALL_DIR "${KDE_INSTALL_LIBEXECDIR}/kf6")
    else()
        set(KIO_WORKER_INSTALL_DIR "libexec/kf6")
    endif()
endif()

## Build both an executable worker and a plugin MODULE so the protocol is
## discoverable by KProtocolInfoFactory (it reads plugin metadata from .so)

add_executable(kio_afp_exec ${kio_afp_sources})
target_include_directories(kio_afp_exec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(kio_afp_exec PRIVATE ${LIBAFPCLIENT_INCLUDE_DIRS})
target_link_libraries(kio_afp_exec PRIVATE
    Qt6::Core
    Qt6::Widgets
    KF6::KIOCore
    KF6::KIOWidgets
    KF6::I18n
)
target_link_libraries(kio_afp_exec PRIVATE ${LIBAFPCLIENT_LIBRARY_PATH})
foreach(_dir IN LISTS LIBAFPCLIENT_LIBRARY_DIRS)
    target_link_options(kio_afp_exec PRIVATE "-Wl,-rpath,${_dir}")
endforeach()
target_compile_options(kio_afp_exec PRIVATE ${LIBAFPCLIENT_CFLAGS_OTHER})
target_compile_definitions(kio_afp_exec PRIVATE TRANSLATION_DOMAIN=\"kio_afp\")
install(TARGETS kio_afp_exec
    RUNTIME DESTINATION ${KIO_WORKER_INSTALL_DIR})

# Shared plugin so KIO will discover 'afp' via embedded metadata
add_library(kio_afp MODULE ${kio_afp_sources} kio_afp_plugin.cpp)
target_include_directories(kio_afp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(kio_afp PRIVATE ${LIBAFPCLIENT_INCLUDE_DIRS})
target_link_libraries(kio_afp PRIVATE
    Qt6::Core
    Qt6::Widgets
    KF6::KIOCore
    KF6::KIOWidgets
    KF6::I18n
)
target_link_libraries(kio_afp PRIVATE ${LIBAFPCLIENT_LIBRARY_PATH})
foreach(_dir IN LISTS LIBAFPCLIENT_LIBRARY_DIRS)
    target_link_options(kio_afp PRIVATE "-Wl,-rpath,${_dir}")
endforeach()
target_compile_options(kio_afp PRIVATE ${LIBAFPCLIENT_CFLAGS_OTHER})
target_compile_definitions(kio_afp PRIVATE TRANSLATION_DOMAIN=\"kio_afp\")

install(TARGETS kio_afp
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt6/plugins/kf6/kio)

# AFP connect helper app (GUI + headless CLI)
add_executable(afp_connect
    afp_connect.cpp
    afploginwidget.cpp
)
target_link_libraries(afp_connect PRIVATE
    Qt6::Core
    Qt6::Widgets
    KF6::I18n
)
target_link_libraries(afp_connect PRIVATE ${LIBAFPCLIENT_LIBRARY_PATH})
foreach(_dir IN LISTS LIBAFPCLIENT_LIBRARY_DIRS)
    target_link_options(afp_connect PRIVATE "-Wl,-rpath,${_dir}")
endforeach()
target_include_directories(afp_connect PRIVATE ${LIBAFPCLIENT_INCLUDE_DIRS})
target_compile_definitions(afp_connect PRIVATE TRANSLATION_DOMAIN=\"kio_afp\")
install(TARGETS afp_connect
    RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    enable_testing()
    add_test(NAME afp_connect_integration
        COMMAND ${CMAKE_SOURCE_DIR}/tests/integration/test_afp_connect.sh)
    set_tests_properties(afp_connect_integration PROPERTIES TIMEOUT 120)
    add_test(NAME afp_get_stat_integration
        COMMAND ${CMAKE_SOURCE_DIR}/tests/integration/test_afp_get_stat.sh)
    set_tests_properties(afp_get_stat_integration PROPERTIES TIMEOUT 120)
endif()
