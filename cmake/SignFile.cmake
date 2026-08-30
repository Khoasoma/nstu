foreach(required NSTU_SIGN_INPUT NSTU_SIGNTOOL_EXECUTABLE
                 NSTU_SIGN_CERT_SHA1 NSTU_SIGN_TIMESTAMP_URL)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
if(NOT EXISTS "${NSTU_SIGN_INPUT}")
    message(FATAL_ERROR "Signing input does not exist: ${NSTU_SIGN_INPUT}")
endif()
execute_process(
    COMMAND "${NSTU_SIGNTOOL_EXECUTABLE}" sign
        /sha1 "${NSTU_SIGN_CERT_SHA1}"
        /fd SHA256
        /tr "${NSTU_SIGN_TIMESTAMP_URL}"
        /td SHA256
        "${NSTU_SIGN_INPUT}"
    RESULT_VARIABLE sign_result)
if(NOT sign_result EQUAL 0)
    message(FATAL_ERROR "signtool failed with exit code ${sign_result}")
endif()
