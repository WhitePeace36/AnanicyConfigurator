cmake_minimum_required(VERSION 3.16)
project(ananicy-gui VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_executable(ananicy-gui
    src/main.cpp
    src/ananicydata.h
    src/ananicydata.cpp
    src/fieldspecs.h
    src/entrymodel.h
    src/entrymodel.cpp
    src/elidedlabel.h
    src/entryeditor.h
    src/entryeditor.cpp
    src/entrydialog.h
    src/entrydialog.cpp
    src/columnfitter.h
    src/columnfitter.cpp
    src/processdialog.h
    src/processdialog.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    resources.qrc
)

target_link_libraries(ananicy-gui PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)
# the version is defined once, in project() above
target_compile_definitions(ananicy-gui PRIVATE APP_VERSION="${PROJECT_VERSION}")

install(TARGETS ananicy-gui RUNTIME DESTINATION bin)
install(FILES ananicy-gui.desktop DESTINATION share/applications)
install(FILES icons/ananicy-configurator.svg DESTINATION share/icons/hicolor/scalable/apps)
foreach(size 16 22 24 32 48 64 128 256)
    install(FILES icons/hicolor/${size}x${size}/apps/ananicy-configurator.png
            DESTINATION share/icons/hicolor/${size}x${size}/apps)
endforeach()

# --- Packages for Debian/Ubuntu (.deb) and Fedora (.rpm) with CPack -------------------
# Build the .deb on Debian/Ubuntu and the .rpm on Fedora, so the dependencies match that
# distribution's Qt packages:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
#   cd build && cpack            # or: cpack -G DEB / cpack -G RPM
# Arch Linux uses the PKGBUILD instead (CPack has no pacman generator).

set(CPACK_PACKAGE_NAME "ananicy-configurator")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "WhitePeace36")
set(CPACK_PACKAGE_CONTACT "WhitePeace36 <timon2201@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Qt GUI to edit ananicy / ananicy-cpp types, rules and ananicy.conf")
set(CPACK_PACKAGE_DESCRIPTION "Graphical editor for the ananicy and ananicy-cpp configuration in /etc/ananicy.d: types, rules (with a picker for running processes) and ananicy.conf. Saves through pkexec and restarts the service.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/WhitePeace36/AnanicyConfigurator")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

# Only offer the generators whose tools are available on this machine.
set(CPACK_GENERATOR "")
find_program(DPKG_DEB_EXECUTABLE dpkg-deb)
find_program(DPKG_SHLIBDEPS_EXECUTABLE dpkg-shlibdeps)
find_program(RPMBUILD_EXECUTABLE rpmbuild)
if(DPKG_DEB_EXECUTABLE)
    list(APPEND CPACK_GENERATOR "DEB")
endif()
if(RPMBUILD_EXECUTABLE)
    list(APPEND CPACK_GENERATOR "RPM")
endif()
if(NOT CPACK_GENERATOR)
    set(CPACK_GENERATOR "TGZ")
endif()

# Debian / Ubuntu
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)          # ananicy-configurator_1.0.0-1_amd64.deb
set(CPACK_DEBIAN_PACKAGE_RELEASE 1)
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
# Qt and C++ library dependencies are computed from the binary by dpkg-shlibdeps.
if(DPKG_SHLIBDEPS_EXECUTABLE)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()
set(CPACK_DEBIAN_PACKAGE_DEPENDS "pkexec | policykit-1, systemd, hicolor-icon-theme")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "ananicy-cpp")

# Fedora
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)             # ananicy-configurator-1.0.0-1.x86_64.rpm
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-2.0-only")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
# Qt and C++ library dependencies are added automatically by rpmbuild.
set(CPACK_RPM_PACKAGE_REQUIRES "polkit, systemd, hicolor-icon-theme")
set(CPACK_RPM_PACKAGE_SUGGESTS "ananicy-cpp")
# Don't claim ownership of directories that belong to other packages.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications
    /usr/share/icons
    /usr/share/icons/hicolor
    /usr/share/icons/hicolor/scalable
    /usr/share/icons/hicolor/scalable/apps)
foreach(size 16 22 24 32 48 64 128 256)
    list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        /usr/share/icons/hicolor/${size}x${size}
        /usr/share/icons/hicolor/${size}x${size}/apps)
endforeach()

include(CPack)
