cmake_minimum_required(VERSION 3.25)
project(nstu_native_uia_probe LANGUAGES CXX RC)
if(NOT WIN32)
    message(FATAL_ERROR "This bounded probe requires Windows UI Automation")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/probe.manifest" [=[
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges>
    <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
  </requestedPrivileges></security></trustInfo>
  <application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>
    <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
    <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
  </windowsSettings></application>
</assembly>
]=])
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/probe.rc"
    "1 24 \"${CMAKE_CURRENT_BINARY_DIR}/probe.manifest\"\n")
add_executable(nstu-native-uia-probe WIN32 main.cpp tests.cpp)
target_sources(nstu-native-uia-probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/probe.rc")
target_compile_features(nstu-native-uia-probe PRIVATE cxx_std_20)
set_target_properties(nstu-native-uia-probe PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(nstu-native-uia-probe PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(nstu-native-uia-probe PRIVATE
    uiautomationcore ole32 oleaut32 uuid user32 gdi32)
if(MSVC)
    target_compile_options(nstu-native-uia-probe PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/permissive-;/UNDEBUG;/utf-8>")
    set_property(TARGET nstu-native-uia-probe PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
else()
    target_compile_options(nstu-native-uia-probe PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror;-UNDEBUG>")
    target_link_options(nstu-native-uia-probe PRIVATE -static -static-libgcc -static-libstdc++)
endif()
enable_testing()
add_test(NAME native_uia_own_window COMMAND nstu-native-uia-probe --self-test)
set_tests_properties(native_uia_own_window PROPERTIES TIMEOUT 90)
add_test(NAME native_uia_fault_paths COMMAND nstu-native-uia-probe --fault-test)
set_tests_properties(native_uia_fault_paths PROPERTIES TIMEOUT 75)
add_test(NAME native_uia_delayed_start COMMAND nstu-native-uia-probe --regression-idle)
add_test(NAME native_uia_event_exception COMMAND nstu-native-uia-probe --regression-event)
add_test(NAME native_uia_diagnostic_lock COMMAND nstu-native-uia-probe --regression-diagnostic)
add_test(NAME native_uia_retained_metadata COMMAND nstu-native-uia-probe --regression-metadata)
add_test(NAME native_uia_retirement_containment COMMAND nstu-native-uia-probe --retirement-test)
set_tests_properties(native_uia_delayed_start native_uia_event_exception native_uia_diagnostic_lock native_uia_retained_metadata
    native_uia_retirement_containment PROPERTIES TIMEOUT 30)

add_test(NAME native_uia_focus_isolated COMMAND nstu-native-uia-probe --focus-isolated-test)
add_test(NAME native_uia_focus_direct_interleave COMMAND nstu-native-uia-probe --focus-competing-test)
add_test(NAME native_uia_focus_os_interleave COMMAND nstu-native-uia-probe --focus-os-interleave-test)
set_tests_properties(native_uia_focus_isolated native_uia_focus_direct_interleave native_uia_focus_os_interleave
    PROPERTIES TIMEOUT 30 LABELS "own-process;focus-diagnostic;not-historical-cause")
