cmake_minimum_required(VERSION 3.16)
project(textwarden VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(textwarden main.cpp mainwindow.cpp)
target_compile_options(textwarden PRIVATE -Wall -Wextra -pedantic)
# Qt Widgets intentionally NOT required. mainwindow.ui retained but unused.
install(TARGETS textwarden RUNTIME DESTINATION bin)
