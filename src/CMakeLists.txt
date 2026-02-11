set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(kio_afp_sources
    kafp_worker.cpp
)

# Find libafpclient via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAFPCLIENT REQUIRED libafpclient)
pkg_check_modules(LIBAFPSL REQUIRED libafpsl)

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

find_library(LIBAFPSL_LIBRARY_PATH
    NAMES afpsl
    PATHS ${LIBAFPSL_LIBRARY_DIRS}
    NO_DEFAULT_PATH
)
if (NOT LIBAFPSL_LIBRARY_PATH)
    message(FATAL_ERROR "Could not locate libafpsl in ${LIBAFPSL_LIBRARY_DIRS}")
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
target_include_directories(kio_afp_exec PRIVATE ${LIBAFPCLIENT_INCLUDE_DIRS} ${LIBAFPSL_INCLUDE_DIRS})
target_link_libraries(kio_afp_exec PRIVATE
    Qt6::Core
    KF6::KIOCore
    KF6::I18n
)
target_link_libraries(kio_afp_exec PRIVATE ${LIBAFPCLIENT_LIBRARY_PATH} ${LIBAFPSL_LIBRARY_PATH})
foreach(_dir IN LISTS LIBAFPCLIENT_LIBRARY_DIRS LIBAFPSL_LIBRARY_DIRS)
    target_link_options(kio_afp_exec PRIVATE "-Wl,-rpath,${_dir}")
endforeach()
target_compile_options(kio_afp_exec PRIVATE ${LIBAFPCLIENT_CFLAGS_OTHER} ${LIBAFPSL_CFLAGS_OTHER})
target_compile_definitions(kio_afp_exec PRIVATE TRANSLATION_DOMAIN=\"kio_afp\")
install(TARGETS kio_afp_exec
    RUNTIME DESTINATION ${KIO_WORKER_INSTALL_DIR})

# Shared plugin so KIO will discover 'afp' via embedded metadata
add_library(kio_afp MODULE ${kio_afp_sources} kio_afp_plugin.cpp)
target_include_directories(kio_afp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(kio_afp PRIVATE ${LIBAFPCLIENT_INCLUDE_DIRS} ${LIBAFPSL_INCLUDE_DIRS})
target_link_libraries(kio_afp PRIVATE
    Qt6::Core
    KF6::KIOCore
    KF6::I18n
)
target_link_libraries(kio_afp PRIVATE ${LIBAFPCLIENT_LIBRARY_PATH} ${LIBAFPSL_LIBRARY_PATH})
foreach(_dir IN LISTS LIBAFPCLIENT_LIBRARY_DIRS LIBAFPSL_LIBRARY_DIRS)
    target_link_options(kio_afp PRIVATE "-Wl,-rpath,${_dir}")
endforeach()
target_compile_options(kio_afp PRIVATE ${LIBAFPCLIENT_CFLAGS_OTHER} ${LIBAFPSL_CFLAGS_OTHER})
target_compile_definitions(kio_afp PRIVATE TRANSLATION_DOMAIN=\"kio_afp\")

install(TARGETS kio_afp
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt6/plugins/kf6/kio)
