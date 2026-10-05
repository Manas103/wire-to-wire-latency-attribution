cmake_minimum_required(VERSION 3.15)
project(wire_to_wire_latency_attribution CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

include_directories(include)

find_program(CLANG_EXECUTABLE clang REQUIRED)
set(XDP_BPF_OBJ ${CMAKE_BINARY_DIR}/xdp_redirect.bpf.o)
add_custom_command(
  OUTPUT ${XDP_BPF_OBJ}
  COMMAND ${CLANG_EXECUTABLE} -O2 -g -target bpf -D__TARGET_ARCH_x86
          -I/usr/include/x86_64-linux-gnu
          -c ${CMAKE_CURRENT_SOURCE_DIR}/src/xdp_redirect.bpf.c -o ${XDP_BPF_OBJ}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/xdp_redirect.bpf.c
  COMMENT "Compiling xdp_redirect.bpf.c to BPF bytecode"
)
add_custom_target(xdp_bpf_obj DEPENDS ${XDP_BPF_OBJ})

add_executable(server src/server.cpp)
add_dependencies(server xdp_bpf_obj)
target_compile_definitions(server PRIVATE XDP_BPF_OBJ_PATH="${XDP_BPF_OBJ}")
target_link_libraries(server bpf elf z)

add_executable(client src/client.cpp)

enable_testing()
add_executable(invariants_test tests/invariants_test.cpp)
add_test(NAME invariants_test COMMAND invariants_test)
