cmake_minimum_required(VERSION 3.16)
project(ananicy-gui VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

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
)

target_link_libraries(ananicy-gui PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)

install(TARGETS ananicy-gui RUNTIME DESTINATION bin)
install(FILES ananicy-gui.desktop DESTINATION share/applications)
