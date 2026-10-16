# Wayland protocol code generation for the SDE's own clients.
#
#   sde_wayland_client_protocol(<out-var> NAME <name> XML <file>)
#
# generates <name>-client-protocol.h and <name>-protocol.c in the current binary
# directory with wayland-scanner and appends the .c file to <out-var>. List it in
# a target's sources and the header is generated before anything that includes
# it compiles, which the hand-written build scripts never guaranteed. A protocol
# shared by several targets in one directory is generated once.

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_get_variable(WAYLAND_PROTOCOLS_DATADIR wayland-protocols pkgdatadir)
if (NOT WAYLAND_PROTOCOLS_DATADIR)
    message(FATAL_ERROR "wayland-protocols is required (pkg-config wayland-protocols)")
endif()

function(sde_wayland_client_protocol out_var)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "NAME;XML" "")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${ARG_NAME}-client-protocol.h")
    set(code "${CMAKE_CURRENT_BINARY_DIR}/${ARG_NAME}-protocol.c")
    get_property(generated DIRECTORY PROPERTY SDE_GENERATED_PROTOCOLS)
    if (NOT ARG_NAME IN_LIST generated)
        add_custom_command(
            OUTPUT "${header}" "${code}"
            COMMAND "${WAYLAND_SCANNER}" client-header "${ARG_XML}" "${header}"
            COMMAND "${WAYLAND_SCANNER}" private-code "${ARG_XML}" "${code}"
            DEPENDS "${ARG_XML}"
            COMMENT "Generating the ${ARG_NAME} client protocol"
            VERBATIM)
        set_property(DIRECTORY APPEND PROPERTY SDE_GENERATED_PROTOCOLS "${ARG_NAME}")
    endif()
    set(${out_var} ${${out_var}} "${header}" "${code}" PARENT_SCOPE)
endfunction()
