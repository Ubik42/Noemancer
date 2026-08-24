if(NOT DEFINED SOURCE_DIR OR NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "SOURCE_DIR and PATCH_FILE are required")
endif()

execute_process(
    COMMAND git apply --check --whitespace=nowarn "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE apply_check
    OUTPUT_QUIET ERROR_QUIET)
if(apply_check EQUAL 0)
    execute_process(
        COMMAND git apply --whitespace=nowarn "${PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE apply_result)
    if(NOT apply_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${PATCH_FILE}")
    endif()
    return()
endif()

execute_process(
    COMMAND git apply --reverse --check --whitespace=nowarn "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE reverse_check
    OUTPUT_QUIET ERROR_QUIET)
if(NOT reverse_check EQUAL 0)
    message(FATAL_ERROR "${PATCH_FILE} is neither applicable nor already applied in ${SOURCE_DIR}")
endif()
