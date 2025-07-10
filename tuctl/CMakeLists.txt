set(SOURCES
  tuctl.c
  auto_pin.c
  log.c
  uid_map.c
)

get_filename_component(PROG_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

include_directories(../bpf)
add_definitions(-DPROG_NAME=${PROG_NAME})
add_executable(${PROG_NAME} ${SOURCES})

if (DISABLE_BPF_TIMER)
  add_definitions(-DDISABLE_BPF_TIMER=1)
endif()

if (USE_SYSTEM_LIBBPF_BPFTOOL)
  set (LIBBPF bpf)
else()
  set (LIBBPF ${PROJECT_SOURCE_DIR}/libbpf/src/root/usr/lib64/libbpf.a elf z)
  include_directories(../libbpf/src/root/usr/include)
endif()

target_link_libraries(${PROG_NAME} common pthread ${LIBBPF})

install(TARGETS ${PROG_NAME} RUNTIME DESTINATION sbin)
# vim: set sw=2 expandtab :
