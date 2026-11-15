cmake_minimum_required(VERSION 3.19)

project(shred-gui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets DBus)
qt_standard_project_setup()

include(CTest)
include(GNUInstallDirs)

# GUI-independent logic (options, scanning, running shred) - used by the application and the tests.
add_library(shredcore STATIC
    src/localpath.h src/localpath.cpp
    src/shredoptions.h src/shredoptions.cpp
    src/shredjob.h src/shredjob.cpp
    src/storagecheck.h src/storagecheck.cpp
    src/deviceerase.h src/deviceerase.cpp
    src/speedhistory.h src/speedhistory.cpp
    src/filelisting.h src/filelisting.cpp
    src/runlog.h src/runlog.cpp
    src/combinedprogress.h src/combinedprogress.cpp
)
target_include_directories(shredcore PUBLIC src)
target_link_libraries(shredcore PUBLIC Qt6::Core)
target_compile_definitions(shredcore PUBLIC SHRED_GUI_VERSION="${PROJECT_VERSION}")

qt_add_executable(shred-gui
    src/main.cpp
    src/mainwindow.h src/mainwindow.cpp
    src/desktopprogress.h src/desktopprogress.cpp
    src/speedgraph.h src/speedgraph.cpp
    src/filedialog.h src/filedialog.cpp
    src/elidedlabel.h src/elidedlabel.cpp
    src/deviceerasedialog.h src/deviceerasedialog.cpp
)
target_link_libraries(shred-gui PRIVATE shredcore Qt6::Widgets Qt6::DBus)

option(SHRED_GUI_INSTALL_SERVICEMENUS "Install the Dolphin (KDE) context menu entries" ON)

# Presets shipped with the application. They are also copied next to the executable in the build
# directory, so that `./build/shred-gui --preset <name>` works without installing.
set(SHRED_GUI_PRESETS
    presets/shred-delete.json
    presets/shred-overwrite.json
    presets/shred-ssd.json
)
foreach(preset IN LISTS SHRED_GUI_PRESETS)
    configure_file(${preset} ${CMAKE_CURRENT_BINARY_DIR}/${preset} COPYONLY)
endforeach()

install(TARGETS shred-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The .desktop files get the absolute path of the installed executable, so that Dolphin and the
# launcher find it even when <prefix>/bin is not in their PATH (e.g. ~/.local/bin before re-login).
# They are generated at install time, because `cmake --install --prefix` may change the prefix.
set(SHRED_GUI_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/packaging)
install(CODE "
    set(SHRED_GUI_EXECUTABLE \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/shred-gui\")
    foreach(file shred-gui.desktop servicemenus/shred-gui-delete.desktop servicemenus/shred-gui-overwrite.desktop servicemenus/shred-gui-ssd.desktop)
        configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/packaging/\${file}.in\"
                       \"${SHRED_GUI_GENERATED_DIR}/\${file}\" @ONLY)
    endforeach()
")
install(FILES ${SHRED_GUI_GENERATED_DIR}/shred-gui.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${SHRED_GUI_PRESETS} DESTINATION ${CMAKE_INSTALL_DATADIR}/shred-gui/presets)
if(SHRED_GUI_INSTALL_SERVICEMENUS)
    # KF6 ignores service menus without the executable bit in user directories.
    install(FILES
        ${SHRED_GUI_GENERATED_DIR}/servicemenus/shred-gui-delete.desktop
        ${SHRED_GUI_GENERATED_DIR}/servicemenus/shred-gui-overwrite.desktop
        ${SHRED_GUI_GENERATED_DIR}/servicemenus/shred-gui-ssd.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/kio/servicemenus
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )
endif()

# Debian package: tools/make-deb.sh (cpack -G DEB in a Release build on a file system with Unix permissions,
# see README, "Debian package"). The package installs to /usr; the .desktop files then point to /usr/bin/shred-gui. The library dependencies (Qt) are
# found by dpkg-shlibdeps.
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME shred-gui)
set(CPACK_PACKAGE_VENDOR fapped)
set(CPACK_PACKAGE_CONTACT "fapped <5470801+fapped@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Qt GUI for shred: securely overwrite and delete files, with a Dolphin context menu")
string(CONCAT CPACK_DEBIAN_PACKAGE_DESCRIPTION
    "A graphical front end for GNU shred: files and directories to overwrite and remove,\n"
    "the shred options, progress per file and overall, Plasma task manager and notification\n"
    "progress, presets and a Dolphin context menu.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/fapped/shred-gui")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "coreutils")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-wayland, pkexec")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "dolphin, nvme-cli, hdparm, cryptsetup")
include(CPack)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(tst_shredcore tests/tst_shredcore.cpp)
    target_link_libraries(tst_shredcore PRIVATE shredcore Qt6::Test)
    target_compile_definitions(tst_shredcore PRIVATE
        SHRED_GUI_SOURCE_PRESETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/presets"
        SHRED_GUI_SOURCE_SERVICEMENUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/packaging/servicemenus")
    add_test(NAME tst_shredcore COMMAND tst_shredcore)
endif()
