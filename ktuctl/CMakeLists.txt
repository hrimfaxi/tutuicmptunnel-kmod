find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

bison_target(ScriptParser script_parser.y ${CMAKE_CURRENT_BINARY_DIR}/script_parser.c COMPILE_FLAGS "-d -Wcounterexamples")
flex_target(ScriptScanner script_lexer.l ${CMAKE_CURRENT_BINARY_DIR}/script_lexer.c)
add_flex_bison_dependency(ScriptScanner ScriptParser)

bison_target(UidParser uid_map_parser.y ${CMAKE_CURRENT_BINARY_DIR}/uid_map_parser.c COMPILE_FLAGS "-d -Wcounterexamples")
flex_target(UidScanner uid_map_lexer.l ${CMAKE_CURRENT_BINARY_DIR}/uid_map_lexer.c)
add_flex_bison_dependency(UidScanner UidParser)

set(SOURCES
  tuctl.c
  log.c
  uid_map.c
)

get_filename_component(PROG_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

include_directories(../common)
include_directories(../kmod)
add_definitions(-DPROG_NAME=${PROG_NAME})
add_executable(${PROG_NAME} ${SOURCES}
  ${BISON_ScriptParser_OUTPUTS} ${FLEX_ScriptScanner_OUTPUTS}
  ${BISON_UidParser_OUTPUTS} ${FLEX_UidScanner_OUTPUTS}
)

target_link_libraries(${PROG_NAME} common pthread)

install(TARGETS ${PROG_NAME} RUNTIME DESTINATION sbin)

# vim: set sw=2 ts=2 expandtab:
