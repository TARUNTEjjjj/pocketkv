# Build a separate project against the installed package, not source headers.
function(run_checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Consumer integration failed (${status}):\n${out}\n${err}")
    endif()
endfunction()

set(prefix "${POCKET_BUILD_DIR}/consumer-install")
set(consumer_build "${POCKET_BUILD_DIR}/consumer-build")
run_checked("${CMAKE_COMMAND}" --install "${POCKET_BUILD_DIR}" --prefix "${prefix}" --config "${POCKET_CONFIG}")
run_checked("${CMAKE_COMMAND}" -S "${POCKET_SOURCE_DIR}/tests/consumer" -B "${consumer_build}"
    "-DCMAKE_PREFIX_PATH=${prefix}" "-DCMAKE_CXX_COMPILER=${POCKET_COMPILER}" "-DCMAKE_BUILD_TYPE=${POCKET_CONFIG}")
run_checked("${CMAKE_COMMAND}" --build "${consumer_build}" --config "${POCKET_CONFIG}")
run_checked("${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build}" --build-config "${POCKET_CONFIG}" --output-on-failure)
