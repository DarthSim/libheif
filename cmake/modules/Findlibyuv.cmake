include(LibFindMacros)

libfind_pkg_check_modules(LIBYUV_PKGCONF libyuv)

find_path(LIBYUV_INCLUDE_DIR
    NAMES libyuv.h
    HINTS ${LIBYUV_PKGCONF_INCLUDE_DIRS} ${LIBYUV_PKGCONF_INCLUDEDIR}
    PATH_SUFFIXES LIBYUV
)

find_library(LIBYUV_LIBRARY
    NAMES yuv
    HINTS ${LIBYUV_PKGCONF_LIBRARY_DIRS} ${LIBYUV_PKGCONF_LIBDIR}
)

set(LIBYUV_PROCESS_LIBS LIBYUV_LIBRARY)
set(LIBYUV_PROCESS_INCLUDES LIBYUV_INCLUDE_DIR)
libfind_process(LIBYUV)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libyuv
    REQUIRED_VARS
        LIBYUV_INCLUDE_DIR
        LIBYUV_LIBRARIES
)
