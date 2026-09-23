# The Wii's own modules, for a runtime build to compile alongside libdol-nx.
#
# These are not a library of their own: they answer a game's SDK calls, and that
# means they use the runtime's memory, its logging and its HLE contracts. A
# build includes this file, adds WIINX_WII_SOURCES to the runtime's sources, and
# puts WIINX_WII_INCLUDE_DIRS on the include path.
#
#   include("${WIINX_WII_DIR}/cmake/Sources.cmake")
#
# libdol-nx's own runtime build does exactly that when it finds this repository
# beside it.
set(WIINX_WII_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

file(GLOB_RECURSE WIINX_WII_SOURCES CONFIGURE_DEPENDS
    "${WIINX_WII_ROOT}/src/*.cpp")

set(WIINX_WII_INCLUDE_DIRS
    "${WIINX_WII_ROOT}/src/nand"
    "${WIINX_WII_ROOT}/src/ios"
    "${WIINX_WII_ROOT}/src/input"
    "${WIINX_WII_ROOT}/src/system")
