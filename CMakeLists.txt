cmake_minimum_required(VERSION 3.24)
project(
  trans
  VERSION 1.2.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
include(CTest)

find_package(
  Qt6 6.5 REQUIRED
  COMPONENTS Core
             Gui
             Widgets
             Network
             Qml
             Quick
             QuickControls2)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Qt6 REQUIRED COMPONENTS DBus)
  find_package(KF6GlobalAccel REQUIRED)
  find_package(KF6WindowSystem REQUIRED)
elseif(WIN32)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_SYSTEM_PROCESSOR MATCHES
                                        "^(ARM64|arm64|aarch64)$")
    message(FATAL_ERROR "The Windows backend requires x64.")
  endif()
  if(Qt6_VERSION VERSION_LESS 6.8)
    message(
      FATAL_ERROR "Windows builds require Qt 6.8 or newer; CI uses Qt 6.8.3.")
  endif()
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
                          WINVER=0x0A00 _WIN32_WINNT=0x0A00)
else()
  message(FATAL_ERROR "Trans supports Linux and Windows x64 backends.")
endif()
qt_standard_project_setup(REQUIRES 6.5)
set(QT_QML_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qml")
if(MSVC)
  add_compile_options(/utf-8 /W4)
endif()

qt_add_library(
  trans_platform
  STATIC
  src/screenshot.h
  src/region_capture.h
  src/region_capture.cpp
  src/platform/platform_error.h
  src/platform/platform_services.h
  src/platform/platform_services.cpp
  src/platform/selection_reader.h
  src/platform/shortcut_service.h
  src/platform/screenshot_service.h
  src/platform/window_integration.h
  src/platform/instance_channel.h
  src/platform/settings_storage.h)
target_include_directories(trans_platform PUBLIC src)
target_link_libraries(trans_platform PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  qt_add_library(
    trans_platform_linux
    STATIC
    src/platform/linux/selection_reader_linux.cpp
    src/platform/linux/shortcut_service_linux.cpp
    src/platform/linux/window_integration_linux.cpp
    src/platform/linux/platform_services_linux.cpp
    src/platform/linux/screenshot_service_linux.cpp
    src/platform/linux/portal_screenshot.h
    src/platform/linux/portal_screenshot.cpp
    src/platform/linux/x11_screenshot.h
    src/platform/linux/x11_screenshot.cpp
    src/platform/linux/instance_channel_linux.h
    src/platform/linux/instance_channel_linux.cpp
    src/platform/linux/settings_storage_linux.cpp)
  target_include_directories(trans_platform_linux PUBLIC src)
  target_link_libraries(
    trans_platform_linux
    PRIVATE trans_platform
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            Qt6::Network
            Qt6::DBus
            KF6::GlobalAccel
            KF6::WindowSystem)
  set(trans_native_target trans_platform_linux)
elseif(WIN32)
  qt_add_library(
    trans_platform_windows
    STATIC
    src/platform/windows/platform_services_windows.cpp
    src/platform/windows/shortcut_service_windows.cpp
    src/platform/windows/window_integration_windows.cpp
    src/platform/windows/source_context_windows.h
    src/platform/windows/selection_reader_windows.cpp
    src/platform/windows/selection_protocol_windows.h
    src/platform/windows/screenshot_service_windows.cpp
    src/platform/windows/instance_channel_windows.cpp
    src/platform/windows/settings_storage_windows.cpp)
  target_include_directories(trans_platform_windows PUBLIC src)
  target_link_libraries(
    trans_platform_windows
    PRIVATE trans_platform
            Qt6::Network
            user32
            advapi32
            dwmapi
            ole32
            uuid)
  set(trans_native_target trans_platform_windows)
  qt_add_executable(trans_selection_helper
                    src/platform/windows/selection_helper_main.cpp)
  target_include_directories(trans_selection_helper PRIVATE src)
  target_link_libraries(
    trans_selection_helper PRIVATE Qt6::Core ole32 oleaut32 uiautomationcore
                                   user32 advapi32)
  install(TARGETS trans_selection_helper
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

qt_add_library(trans_core STATIC)
qt_add_qml_module(
  trans_core
  URI
  Trans.Core
  VERSION
  1.0
  SOURCES
  src/translation.h
  src/translation.cpp
  src/providers.h
  src/providers.cpp
  src/provider_tools.h
  src/provider_tools.cpp
  src/settings.h
  src/settings.cpp
  src/controller.h
  src/controller.cpp
  src/ocr.h
  src/ocr.cpp
  src/desktop.h
  src/desktop.cpp)
target_include_directories(trans_core PUBLIC src)
target_link_libraries(
  trans_core
  PUBLIC Qt6::Core
         Qt6::Gui
         Qt6::Widgets
         Qt6::Network
         Qt6::Qml
         Qt6::Quick
         trans_platform
  PRIVATE ${trans_native_target})
if(MSVC)
  target_compile_options(trans_core PRIVATE /W4)
else()
  target_compile_options(trans_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

qt_add_executable(trans src/main.cpp)
target_compile_definitions(trans PRIVATE TRANS_VERSION="${PROJECT_VERSION}")
if(WIN32)
  set_target_properties(trans PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MSVC)
    target_link_options(trans PRIVATE /MANIFEST:NO)
  endif()
  configure_file(data/trans.rc.in "${CMAKE_CURRENT_BINARY_DIR}/trans.rc" @ONLY)
  target_sources(trans PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/trans.rc")
  add_dependencies(trans trans_selection_helper)
endif()
set(trans_qml_files
    qml/Main.qml
    qml/SettingsWindow.qml
    qml/ProviderPage.qml
    qml/TranslationPage.qml
    qml/DesktopPage.qml
    qml/OcrPage.qml
    qml/UiTheme.qml
    qml/UiIcon.qml
    qml/UiButton.qml
    qml/UiField.qml
    qml/UiCombo.qml
    qml/UiSpin.qml
    qml/UiCheck.qml
    qml/UiTextArea.qml
    qml/UiSection.qml
    qml/UiScrollView.qml)
foreach(qml_file IN LISTS trans_qml_files)
  get_filename_component(qml_name "${qml_file}" NAME)
  set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS
                                                       "${qml_name}")
endforeach()
qt_add_qml_module(
  trans
  URI
  Trans
  VERSION
  1.0
  QML_FILES
  ${trans_qml_files})
qt_add_resources(trans icons PREFIX "/" FILES assets/trans.svg)
target_link_libraries(trans PRIVATE trans_core trans_coreplugin
                                    Qt6::QuickControls2)

install(TARGETS trans RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(FILES data/io.github.trans.Trans.desktop
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES assets/trans.svg
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  qt_add_executable(trans_tests tests/test_trans.cpp)
  target_link_libraries(trans_tests PRIVATE trans_core trans_coreplugin
                                            Qt6::Test Qt6::QuickControls2)
  target_compile_definitions(
    trans_tests PRIVATE TRANS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                        TRANS_VERSION="${PROJECT_VERSION}")
  qt_add_executable(trans_ocr_tests tests/test_ocr.cpp)
  target_link_libraries(trans_ocr_tests PRIVATE trans_core trans_coreplugin
                                                Qt6::Test)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(trans_tests PRIVATE Qt6::DBus)
    target_link_libraries(trans_ocr_tests PRIVATE Qt6::DBus)
  endif()
  add_test(NAME trans_ocr COMMAND trans_ocr_tests -o -,txt -o trans_ocr-test.txt,txt)
  add_test(NAME trans_core COMMAND trans_tests -o -,txt -o trans_core-test.txt,txt)
  add_test(NAME trans_qml_smoke COMMAND trans --smoke-test)
  set_tests_properties(
    trans_core trans_ocr trans_qml_smoke
    PROPERTIES ENVIRONMENT
               "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Fusion" TIMEOUT 30)
  if(WIN32)
    set_property(TEST trans_core trans_ocr trans_qml_smoke APPEND PROPERTY
                 ENVIRONMENT "QT_QPA_FONTDIR=$ENV{WINDIR}/Fonts")
    qt_add_executable(trans_windows_instance_tests
                      tests/test_windows_instance.cpp)
    target_link_libraries(trans_windows_instance_tests
                          PRIVATE trans_platform_windows Qt6::Test Qt6::Network)
    qt_add_executable(trans_windows_storage_tests
                      tests/test_windows_storage.cpp)
    target_link_libraries(trans_windows_storage_tests
                          PRIVATE trans_platform_windows Qt6::Test advapi32)
    add_test(NAME trans_windows_instance COMMAND trans_windows_instance_tests
             -o -,txt -o trans_windows_instance-test.txt,txt)
    add_test(NAME trans_windows_storage COMMAND trans_windows_storage_tests
             -o -,txt -o trans_windows_storage-test.txt,txt)
    set_tests_properties(trans_windows_instance trans_windows_storage
                         PROPERTIES TIMEOUT 120)
  endif()
  # Opt-in: sends real keys on the current KDE X11 desktop, using only temporary
  # test data.
  option(TRANS_X11_TESTS
         "Build interactive KDE X11 regression tests (requires xdotool)" OFF)
  if(TRANS_X11_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    qt_add_executable(trans_x11_tests tests/test_x11_escape.cpp)
    target_link_libraries(
      trans_x11_tests
      PRIVATE trans_core trans_coreplugin Qt6::Test Qt6::QuickControls2
              KF6::GlobalAccel KF6::WindowSystem)
    target_compile_definitions(
      trans_x11_tests PRIVATE TRANS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endif()
