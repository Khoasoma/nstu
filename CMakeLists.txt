cmake_minimum_required(VERSION 3.25)

project(nstu
    VERSION 0.1.0
    DESCRIPTION "Low-latency classroom lab management and screen broadcasting"
    LANGUAGES CXX
)

option(NSTU_BUILD_CLIENT "Build the Windows client" ON)
option(NSTU_BUILD_SERVER "Build the server UI" ON)
option(NSTU_BUILD_TESTS "Build unit and integration tests" ON)
option(NSTU_BUILD_VIDEO "Build DXGI and Media Foundation components" ON)
option(NSTU_ENABLE_WERROR "Treat compiler warnings as errors" OFF)
option(NSTU_ENABLE_PACKAGING "Enable Windows CPack/NSIS packaging" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NstuWarnings)
include(NstuSigning)

add_subdirectory(common)

if(WIN32 AND NSTU_BUILD_VIDEO)
    add_subdirectory(video)
endif()

if(NSTU_BUILD_CLIENT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/client/CMakeLists.txt")
    add_subdirectory(client)
endif()

if(NSTU_BUILD_SERVER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server/CMakeLists.txt")
    add_subdirectory(server)
endif()

if(NSTU_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

foreach(nstu_signable_target
        nstu-server nstu-service nstu-agent nstu-provision nstu_video_probe)
    nstu_sign_target(${nstu_signable_target})
endforeach()

if(WIN32 AND NSTU_ENABLE_PACKAGING)
    include(GNUInstallDirs)

    if(TARGET nstu-server)
        install(TARGETS nstu-server
            RUNTIME DESTINATION server
            COMPONENT server)
    endif()
    if(TARGET nstu-service)
        install(TARGETS nstu-service nstu-agent nstu-provision
            RUNTIME DESTINATION client
            COMPONENT client)
        install(FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/install-client-service.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/uninstall-client-service.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/configure-data-root.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/test-system-setup.ps1"
            DESTINATION client
            COMPONENT client)
    endif()
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.vi.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        DESTINATION docs
        COMPONENT docs)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/configure-data-root.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/new-enrollment-secret.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/test-production-deployment.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/test-system-setup.ps1"
        DESTINATION docs/deployment
        COMPONENT docs)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs/"
        DESTINATION docs/project
        COMPONENT docs)

    set(CPACK_PACKAGE_NAME "nstu")
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(CPACK_PACKAGE_VENDOR "Project NSTU")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "NSTU")
    set(CPACK_NSIS_DISPLAY_NAME "Project NSTU")
    set(CPACK_NSIS_PACKAGE_NAME "Project NSTU")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_COMPONENTS_GROUPING IGNORE)
    set(CPACK_COMPONENT_SERVER_DISPLAY_NAME "NSTU Server")
    set(CPACK_COMPONENT_SERVER_DESCRIPTION "NSTU administration server")
    set(CPACK_COMPONENT_CLIENT_DISPLAY_NAME "NSTU Client")
    set(CPACK_COMPONENT_CLIENT_DESCRIPTION "NSTU service and session agent")
    set(CPACK_COMPONENT_DOCS_DISPLAY_NAME "Documentation and licenses")
    set(CPACK_COMPONENT_DOCS_DESCRIPTION "Project documentation and notices")
    set(CPACK_GENERATOR "NSIS")

    include(CPack)

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/CPackServerConfig.cmake.in"
        "${CMAKE_BINARY_DIR}/CPackServerConfig.cmake" @ONLY)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/CPackClientConfig.cmake.in"
        "${CMAKE_BINARY_DIR}/CPackClientConfig.cmake" @ONLY)

    if(TARGET nstu-server)
        set(nstu_server_package_sign_command)
        if(NSTU_ENABLE_SIGNING)
            list(APPEND nstu_server_package_sign_command
                COMMAND "${CMAKE_COMMAND}"
                    -DNSTU_SIGN_INPUT=${CMAKE_BINARY_DIR}/nstu-server-${PROJECT_VERSION}.exe
                    -DNSTU_SIGNTOOL_EXECUTABLE=${NSTU_SIGNTOOL_EXECUTABLE}
                    -DNSTU_SIGN_CERT_SHA1=${NSTU_SIGN_CERT_SHA1}
                    -DNSTU_SIGN_TIMESTAMP_URL=${NSTU_SIGN_TIMESTAMP_URL}
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SignFile.cmake")
        endif()
        add_custom_target(nstu-package-server
            COMMAND "${CMAKE_CPACK_COMMAND}" --config
                "${CMAKE_BINARY_DIR}/CPackServerConfig.cmake"
            ${nstu_server_package_sign_command}
            DEPENDS nstu-server
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Packaging NSTU server installer")
    endif()
    if(TARGET nstu-service)
        set(nstu_client_package_sign_command)
        if(NSTU_ENABLE_SIGNING)
            list(APPEND nstu_client_package_sign_command
                COMMAND "${CMAKE_COMMAND}"
                    -DNSTU_SIGN_INPUT=${CMAKE_BINARY_DIR}/nstu-client-${PROJECT_VERSION}.exe
                    -DNSTU_SIGNTOOL_EXECUTABLE=${NSTU_SIGNTOOL_EXECUTABLE}
                    -DNSTU_SIGN_CERT_SHA1=${NSTU_SIGN_CERT_SHA1}
                    -DNSTU_SIGN_TIMESTAMP_URL=${NSTU_SIGN_TIMESTAMP_URL}
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SignFile.cmake")
        endif()
        add_custom_target(nstu-package-client
            COMMAND "${CMAKE_CPACK_COMMAND}" --config
                "${CMAKE_BINARY_DIR}/CPackClientConfig.cmake"
            ${nstu_client_package_sign_command}
            DEPENDS nstu-service nstu-agent nstu-provision
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Packaging NSTU client installer")
    endif()
endif()
