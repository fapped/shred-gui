cmake_minimum_required(VERSION 3.19)

project(shred-gui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()

include(CTest)
include(GNUInstallDirs)

# GUI-independent logic (options, scanning, running shred) - used by the application and the tests.
add_library(shredcore STATIC
    src/shredoptions.h src/shredoptions.cpp
    src/shredjob.h src/shredjob.cpp
)
target_include_directories(shredcore PUBLIC src)
target_link_libraries(shredcore PUBLIC Qt6::Core)
target_compile_definitions(shredcore PUBLIC SHRED_GUI_VERSION="${PROJECT_VERSION}")

qt_add_executable(shred-gui
    src/main.cpp
    src/mainwindow.h src/mainwindow.cpp
)
target_link_libraries(shred-gui PRIVATE shredcore Qt6::Widgets)

option(SHRED_GUI_INSTALL_SERVICEMENUS "Install the Dolphin (KDE) context menu entries" ON)

# Presets shipped with the application. They are also copied next to the executable in the build
# directory, so that `./build/shred-gui --preset <name>` works without installing.
set(SHRED_GUI_PRESETS
    presets/shred-delete.json
    presets/shred-overwrite.json
)
foreach(preset IN LISTS SHRED_GUI_PRESETS)
    configure_file(${preset} ${CMAKE_CURRENT_BINARY_DIR}/${preset} COPYONLY)
endforeach()

install(TARGETS shred-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/shred-gui.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${SHRED_GUI_PRESETS} DESTINATION ${CMAKE_INSTALL_DATADIR}/shred-gui/presets)
if(SHRED_GUI_INSTALL_SERVICEMENUS)
    # KF6 ignores service menus without the executable bit in user directories.
    install(FILES
        packaging/servicemenus/shred-gui-delete.desktop
        packaging/servicemenus/shred-gui-overwrite.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/kio/servicemenus
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )
endif()

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(tst_shredcore tests/tst_shredcore.cpp)
    target_link_libraries(tst_shredcore PRIVATE shredcore Qt6::Test)
    target_compile_definitions(tst_shredcore PRIVATE SHRED_GUI_SOURCE_PRESETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/presets")
    add_test(NAME tst_shredcore COMMAND tst_shredcore)
endif()
