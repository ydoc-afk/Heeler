set(BOOST_VERSION 1.87.0)

set(BOOST_COMPONENTS
        log
        container
        locale
        process
        # Align needed by boost::json
        align
        json
        system
)
find_package(Boost ${BOOST_VERSION} COMPONENTS ${BOOST_COMPONENTS} QUIET)
if (NOT Boost_FOUND)
    # Building Boost from source (FetchContent) isn't supported: some dependencies (immer, Simple-Web-Server) run
    # their own find_package(Boost), which can't find a Boost that is only built, not installed.
    list(JOIN BOOST_COMPONENTS ", " BOOST_COMPONENTS_LIST)
    message(FATAL_ERROR
            "Boost >= ${BOOST_VERSION} (components: ${BOOST_COMPONENTS_LIST}) was not found.\n"
            "Install the Boost development packages, for example:\n"
            "  Debian/Ubuntu: apt install libboost-thread-dev libboost-locale-dev libboost-filesystem-dev "
            "libboost-log-dev libboost-stacktrace-dev libboost-container-dev libboost-json-dev\n"
            "  Arch: pacman -S boost\n"
            "  Fedora: dnf install boost-devel\n"
            "or point CMake at an existing installation with -DBoost_ROOT=<prefix>.")
endif ()
include_directories(${Boost_INCLUDE_DIRS})
