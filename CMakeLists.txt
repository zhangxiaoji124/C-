cmake_minimum_required(VERSION 3.20)
project(OrbitOps VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
if(MSVC)
  target_compile_definitions(sqlite3 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(orbit_core
  src/database.cpp
  src/ollama_client.cpp
  src/agent.cpp
  src/worker.cpp
  src/http_server.cpp
)
target_include_directories(orbit_core PUBLIC include third_party third_party/sqlite)
target_link_libraries(orbit_core PUBLIC sqlite3)

if(WIN32)
  target_link_libraries(orbit_core PUBLIC ws2_32)
endif()
if(UNIX)
  target_link_libraries(orbit_core PUBLIC pthread dl)
endif()

add_executable(orbitops src/main.cpp)
target_link_libraries(orbitops PRIVATE orbit_core)

add_executable(orbitops_tests tests/unit_tests.cpp)
target_link_libraries(orbitops_tests PRIVATE orbit_core)

enable_testing()
add_test(NAME orbitops_unit_tests COMMAND orbitops_tests)

install(TARGETS orbitops RUNTIME DESTINATION bin)
install(DIRECTORY web DESTINATION share/orbitops)
