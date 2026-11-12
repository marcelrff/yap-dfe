cmake_minimum_required(VERSION 3.21)
project(yapdfv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_QT6 REQUIRED IMPORTED_TARGET poppler-qt6)

add_library(yapdfv_core STATIC
    src/document.cpp
    src/document.h
    src/findbar.cpp
    src/findbar.h
    src/pdfview.cpp
    src/pdfview.h
    src/tabpane.cpp
    src/tabpane.h
    src/mainwindow.cpp
    src/mainwindow.h
)
target_include_directories(yapdfv_core PUBLIC src)
target_compile_options(yapdfv_core PUBLIC -Wall -Wextra)
target_link_libraries(yapdfv_core PUBLIC Qt6::Widgets Qt6::Concurrent PRIVATE PkgConfig::POPPLER_QT6)

add_executable(yapdfv src/main.cpp)
set_target_properties(yapdfv PROPERTIES OUTPUT_NAME "yap-dfv")
target_link_libraries(yapdfv PRIVATE yapdfv_core)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(smoketest tests/smoketest.cpp)
    target_link_libraries(smoketest PRIVATE yapdfv_core Qt6::Test)
    add_test(NAME smoketest COMMAND smoketest)
    set_tests_properties(smoketest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

include(GNUInstallDirs)
install(TARGETS yapdfv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/yap-dfv.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
