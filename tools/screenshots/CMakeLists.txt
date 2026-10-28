# Builds the application together with screenshot.cpp, which takes the README screenshots from inside
# the running application. Not part of the main build - run ../make-screenshots.sh.
cmake_minimum_required(VERSION 3.19)

project(shred-gui-screenshots LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets DBus)
qt_standard_project_setup()

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
file(GLOB APP_SOURCES ${APP_DIR}/src/*.cpp ${APP_DIR}/src/*.h)

qt_add_executable(shred-gui-screenshots screenshot.cpp ${APP_SOURCES})
target_include_directories(shred-gui-screenshots PRIVATE ${APP_DIR}/src)
target_compile_definitions(shred-gui-screenshots PRIVATE SHRED_GUI_VERSION="screenshots")
target_link_libraries(shred-gui-screenshots PRIVATE Qt6::Widgets Qt6::DBus)

# The presets next to the executable, as in the main build.
file(GLOB PRESETS ${APP_DIR}/presets/*.json)
file(COPY ${PRESETS} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/presets)
