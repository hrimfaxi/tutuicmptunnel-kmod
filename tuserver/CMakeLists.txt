include_directories(${SODIUM_INCLUDE_DIR})

set(CLIENT_SOURCES
  tuctl_client.c
  common.c
  log.c
)
add_executable(tuctl_client ${CLIENT_SOURCES})
target_link_libraries(tuctl_client common pthread ${SODIUM_LIBRARY})
install(TARGETS tuctl_client RUNTIME DESTINATION bin)

set(SERVER_SOURCES
  tuctl_server.c
  common.c
  log.c
)
add_executable(tuctl_server ${SERVER_SOURCES})
target_link_libraries(tuctl_server common pthread ${SODIUM_LIBRARY})
install(TARGETS tuctl_server RUNTIME DESTINATION bin)

# vim: set sw=2 expandtab :
