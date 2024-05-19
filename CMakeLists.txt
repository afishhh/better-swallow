cmake_minimum_required(VERSION 3.10)

project(better-swallow)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(X11 REQUIRED)

add_executable(${PROJECT_NAME} main.cc)

target_compile_options(${PROJECT_NAME} PRIVATE
  -Wall -Wextra
)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
  target_compile_options(${PROJECT_NAME} PRIVATE
    -g -pg
  )
else()
  target_compile_options(${PROJECT_NAME} PRIVATE
    -Werror
  )
endif()

target_link_libraries(${PROJECT_NAME} X11::X11 X11::XRes)

install(
	TARGETS ${PROJECT_NAME}
	RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
