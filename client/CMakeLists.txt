add_library(nstu_client_platform STATIC
    src/agent_protocol.cpp
    src/client_config.cpp
    src/client_control.cpp
    src/named_pipe.cpp
    src/session.cpp
)
target_include_directories(nstu_client_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(nstu_client_platform PUBLIC cxx_std_20)
target_link_libraries(nstu_client_platform PUBLIC nstu_common advapi32 userenv wtsapi32)
nstu_enable_warnings(nstu_client_platform)

add_executable(nstu-service service/main.cpp)
target_link_libraries(nstu-service PRIVATE nstu_client_platform)
nstu_enable_warnings(nstu-service)

add_executable(nstu-provision tools/provision.cpp)
target_link_libraries(nstu-provision PRIVATE nstu_client_platform)
nstu_enable_warnings(nstu-provision)
if(MINGW)
    target_link_options(nstu-provision PRIVATE -municode)
endif()

add_executable(nstu-agent WIN32 agent/main.cpp)
target_link_libraries(nstu-agent PRIVATE nstu_client_platform shell32 user32 gdi32)
nstu_enable_warnings(nstu-agent)

if(MINGW)
    target_link_options(nstu-agent PRIVATE -municode)
endif()
