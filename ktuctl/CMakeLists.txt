find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBMNL REQUIRED libmnl)

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

set(BISON_COMPILE_FLAGS "-d")

# 只有 Bison >= 3.7 才添加 -Wcounterexamples
if(NOT BISON_VERSION VERSION_LESS "3.7")
    string(APPEND BISON_COMPILE_FLAGS " -Wcounterexamples")
endif()

bison_target(ScriptParser script_parser.y ${CMAKE_CURRENT_BINARY_DIR}/script_parser.c COMPILE_FLAGS ${BISON_COMPILE_FLAGS})
flex_target(ScriptScanner script_lexer.l ${CMAKE_CURRENT_BINARY_DIR}/script_lexer.c)
add_flex_bison_dependency(ScriptScanner ScriptParser)

bison_target(UidParser uid_map_parser.y ${CMAKE_CURRENT_BINARY_DIR}/uid_map_parser.c COMPILE_FLAGS ${BISON_COMPILE_FLAGS})
flex_target(UidScanner uid_map_lexer.l ${CMAKE_CURRENT_BINARY_DIR}/uid_map_lexer.c)
add_flex_bison_dependency(UidScanner UidParser)

set(SOURCES
  tuctl.c
  log.c
  uid_map.c
)

get_filename_component(PROG_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

include_directories(../common ../kmod ${CMAKE_CURRENT_SOURCE_DIR})
add_definitions(-DPROG_NAME=${PROG_NAME})
add_executable(${PROG_NAME} ${SOURCES}
  ${BISON_ScriptParser_OUTPUTS} ${FLEX_ScriptScanner_OUTPUTS}
  ${BISON_UidParser_OUTPUTS} ${FLEX_UidScanner_OUTPUTS}
)

target_include_directories(${PROG_NAME} PRIVATE
  ${LIBMNL_INCLUDE_DIRS}
)

target_compile_options(${PROG_NAME} PRIVATE
  ${LIBMNL_CFLAGS_OTHER}
)

target_link_libraries(${PROG_NAME} PRIVATE
  common
  pthread
  ${LIBMNL_LIBRARIES}
)

install(TARGETS ${PROG_NAME} RUNTIME DESTINATION sbin)

# vim: set sw=2 ts=2 expandtab:
