add_library(nstu_video STATIC
    src/color_converter.cpp
    src/desktop_duplication.cpp
    src/hardware_encoder.cpp
    src/recovering_pipeline.cpp
)

target_include_directories(nstu_video PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(nstu_video PUBLIC cxx_std_20)
target_link_libraries(nstu_video PUBLIC
    nstu_common
    d3d11
    dxgi
    mf
    mfplat
    mfuuid
    ole32
)
nstu_enable_warnings(nstu_video)

add_executable(nstu_video_probe tools/video_probe.cpp)
target_link_libraries(nstu_video_probe PRIVATE nstu_video)
nstu_enable_warnings(nstu_video_probe)
