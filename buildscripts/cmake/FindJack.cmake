find_package(PkgConfig QUIET)

if (PkgConfig_FOUND)
    pkg_check_modules(JACK QUIET IMPORTED_TARGET jack)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Jack
    REQUIRED_VARS JACK_FOUND
    VERSION_VAR JACK_VERSION
)

if (Jack_FOUND AND NOT TARGET Jack::Jack)
    add_library(Jack::Jack INTERFACE IMPORTED)
    set_property(TARGET Jack::Jack PROPERTY
        INTERFACE_LINK_LIBRARIES PkgConfig::JACK
    )
endif()
