option(BUILD_BENCHMARKS "Build Sphinx benchmarks" OFF)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/sphinx)

RAGEL_TARGET(protocol src/protocol.rl ${CMAKE_CURRENT_BINARY_DIR}/include/sphinx/protocol.h)

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  set(SYSTEM_SOURCES src/reactor-epoll.cpp)
else()
    message(FATAL_ERROR "Unsupported system: ${CMAKE_SYSTEM_NAME}")
endif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")

include_directories(
    include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

add_executable(sphinxd
    src/MurmurHash3.cpp
    src/logmem.cpp
    src/memory.cpp
    src/reactor.cpp
    src/sphinxd.cpp
    ${SYSTEM_SOURCES}
    ${RAGEL_protocol_OUTPUTS}
)
target_link_libraries(sphinxd pthread)

if(GTEST_FOUND)
  add_executable(sphinxd_tests
      src/logmem.cpp
      test/logmem_test.cpp
      test/main.cpp
      test/protocol_test.cpp
      test/spsc_queue_test.cpp
      test/string_test.cpp
      ${RAGEL_protocol_OUTPUTS}
  )
  target_link_libraries(sphinxd_tests ${GTEST_BOTH_LIBRARIES} pthread)
  add_test(sphinxd_tests sphinxd_tests)
endif()

if(BUILD_BENCHMARKS)
  add_executable(sphinxd_perf
      perf/logmem_perf.cpp
      perf/main.cpp
      perf/protocol_perf.cpp
      src/logmem.cpp
      src/memory.cpp
      ${RAGEL_protocol_OUTPUTS}
  )
  target_link_libraries(sphinxd_perf benchmark pthread)
endif()
