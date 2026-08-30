add_library(nstu_common STATIC
    src/multicast.cpp
    src/network.cpp
    src/packet_loss.cpp
    src/protocol.cpp
)

target_include_directories(nstu_common
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(nstu_common PUBLIC cxx_std_20)
nstu_enable_warnings(nstu_common)

if(WIN32)
    target_link_libraries(nstu_common PUBLIC ws2_32)
endif()
