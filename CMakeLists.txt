cmake_minimum_required(VERSION 3.16)
project(netpkt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Put executables directly in build/ so the documented ./build/video_server works.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio highgui)
find_package(Threads REQUIRED)

# Everything except the two main() files lives in one static library so the
# tests exercise exactly the code the executables run.
add_library(netpkt_core STATIC
  src/common/util.cpp
  src/common/frame.cpp
  src/common/protocol.cpp
  src/net/tcp.cpp
  src/net/udp.cpp
  src/bench/metrics.cpp)
target_include_directories(netpkt_core PUBLIC include)
# SYSTEM: do not show warnings that come from OpenCV's own headers.
target_include_directories(netpkt_core SYSTEM PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(netpkt_core PUBLIC ${OpenCV_LIBS} Threads::Threads)
target_compile_options(netpkt_core PUBLIC -Wall -Wextra -Wpedantic)

add_executable(video_server src/server/main.cpp)
target_link_libraries(video_server PRIVATE netpkt_core)

add_executable(video_client src/client/main.cpp)
target_link_libraries(video_client PRIVATE netpkt_core)

option(NETPKT_BUILD_TESTS "Build unit tests and the loopback integration test" ON)
if(NETPKT_BUILD_TESTS)
  enable_testing()

  function(add_netpkt_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE netpkt_core)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30) # a hang must fail, not wait forever
  endfunction()

  add_netpkt_test(test_protocol)
  add_netpkt_test(test_tcp)
  add_netpkt_test(test_udp)
  add_netpkt_test(test_metrics)
  add_netpkt_test(test_frame)

  # Writes a small synthetic video; used by the integration test and handy
  # when you have no sample video yet.
  add_executable(gen_video tests/gen_video.cpp)
  target_link_libraries(gen_video PRIVATE netpkt_core)

  add_test(NAME integration_loopback
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration_test.sh ${CMAKE_BINARY_DIR})
  set_tests_properties(integration_loopback PROPERTIES TIMEOUT 120)
endif()
