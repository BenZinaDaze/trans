cmake_minimum_required(VERSION 3.24)
project(tran VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
include(CTest)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network DBus Qml Quick QuickControls2)
find_package(KF6GlobalAccel REQUIRED)
find_package(KF6WindowSystem REQUIRED)
qt_standard_project_setup(REQUIRES 6.5)
set(QT_QML_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qml")

qt_add_library(tran_core STATIC)
qt_add_qml_module(tran_core
    URI Tran.Core
    VERSION 1.0
    SOURCES
        src/translation.h src/translation.cpp
        src/providers.h src/providers.cpp
        src/provider_tools.h src/provider_tools.cpp
        src/settings.h src/settings.cpp
        src/controller.h src/controller.cpp
        src/desktop.h src/desktop.cpp
)
target_include_directories(tran_core PUBLIC src)
target_link_libraries(tran_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::DBus Qt6::Qml Qt6::Quick
    KF6::GlobalAccel KF6::WindowSystem
)
target_compile_options(tran_core PRIVATE -Wall -Wextra -Wpedantic)

qt_add_executable(tran src/main.cpp)
target_compile_definitions(tran PRIVATE TRAN_VERSION="${PROJECT_VERSION}")
set(tran_qml_files
    qml/Main.qml qml/SettingsWindow.qml
    qml/ProviderPage.qml qml/TranslationPage.qml qml/DesktopPage.qml
    qml/UiTheme.qml qml/UiIcon.qml qml/UiButton.qml qml/UiField.qml
    qml/UiCombo.qml qml/UiSpin.qml qml/UiCheck.qml qml/UiTextArea.qml qml/UiSection.qml qml/UiScrollView.qml
)
foreach(qml_file IN LISTS tran_qml_files)
    get_filename_component(qml_name "${qml_file}" NAME)
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
endforeach()
qt_add_qml_module(tran
    URI Tran
    VERSION 1.0
    QML_FILES ${tran_qml_files}
)
qt_add_resources(tran icons PREFIX "/" FILES assets/tran.svg)
target_link_libraries(tran PRIVATE tran_core tran_coreplugin Qt6::QuickControls2)

install(TARGETS tran RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/io.github.tran.Tran.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES assets/tran.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(tran_tests tests/test_tran.cpp)
    target_link_libraries(tran_tests PRIVATE tran_core tran_coreplugin Qt6::Test Qt6::QuickControls2)
    target_compile_definitions(tran_tests PRIVATE TRAN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tran_core COMMAND tran_tests)
    add_test(NAME tran_qml_smoke COMMAND tran --smoke-test)
    set_tests_properties(tran_core tran_qml_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        TIMEOUT 30
    )
    # Opt-in: sends real keys on the current KDE X11 desktop, using only temporary test data.
    option(TRAN_X11_TESTS "Build interactive KDE X11 regression tests (requires xdotool)" OFF)
    if(TRAN_X11_TESTS)
        qt_add_executable(tran_x11_tests tests/test_x11_escape.cpp)
        target_link_libraries(tran_x11_tests PRIVATE tran_core tran_coreplugin Qt6::Test Qt6::QuickControls2)
        target_compile_definitions(tran_x11_tests PRIVATE TRAN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()
