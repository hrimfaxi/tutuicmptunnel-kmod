set(SOURCES
  tuctl.c
  log.c
  uid_map.c
)

get_filename_component(PROG_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

include_directories(../bpf)
include_directories(../kmod)
add_definitions(-DPROG_NAME=${PROG_NAME})
add_executable(${PROG_NAME} ${SOURCES})

target_link_libraries(${PROG_NAME} common pthread)

install(TARGETS ${PROG_NAME} RUNTIME DESTINATION sbin)
# vim: set sw=2 expandtab :
