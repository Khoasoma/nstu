if(NOT TARGET nstu_imgui)
    include(FetchContent)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)

    add_library(nstu_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    )
    target_include_directories(nstu_imgui PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
    )
    target_link_libraries(nstu_imgui PUBLIC
        d3d11 dxgi d3dcompiler dwmapi user32 imm32
    )
endif()

add_executable(nstu-setup WIN32
    src/main.cpp
    src/hardware_scan.cpp
    src/driver_scan.cpp
    src/policy_audit.cpp
)
target_include_directories(nstu-setup PRIVATE include)
target_link_libraries(nstu-setup PRIVATE
    nstu_common nstu_imgui d3d11 dxgi d3dcompiler iphlpapi mf mfplat mfuuid
    ole32 setupapi user32
)
if(MSVC)
    target_compile_options(nstu-setup PRIVATE /utf-8)
endif()
nstu_enable_warnings(nstu-setup)
if(MINGW)
    target_link_options(nstu-setup PRIVATE -municode)
endif()
