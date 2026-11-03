# Run the installed CLI and a separate library consumer after moving the prefix.
function(run_checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Consumer integration failed (${status}):\n${out}\n${err}")
    endif()
endfunction()

set(prefix "${POCKET_BUILD_DIR}/consumer-install")
set(relocated_prefix "${POCKET_BUILD_DIR}/consumer-relocated")
set(consumer_build "${POCKET_BUILD_DIR}/consumer-build")
# POCKET_CONFIG is empty when no build type is set. run_checked's unquoted ${ARGV}
# drops empty values, so pass the configuration flags only when it is non-empty.
set(config_args)
set(ctest_config_args)
if(NOT POCKET_CONFIG STREQUAL "")
    set(config_args --config "${POCKET_CONFIG}")
    set(ctest_config_args --build-config "${POCKET_CONFIG}")
endif()
# All three directories are owned by this test. Clear stale exports and package
# locations so an earlier successful installation cannot hide a broken one.
file(REMOVE_RECURSE "${prefix}" "${relocated_prefix}" "${consumer_build}")
run_checked("${CMAKE_COMMAND}" --install "${POCKET_BUILD_DIR}" --prefix "${prefix}" ${config_args})
file(RENAME "${prefix}" "${relocated_prefix}")
set(cli "${relocated_prefix}/${POCKET_BINDIR}/pocketkv")
set(database "${relocated_prefix}/cli-example.db")
run_checked("${cli}" "${database}" put packaging works)
execute_process(COMMAND "${cli}" "${database}" get-raw packaging
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT status EQUAL 0 OR NOT out STREQUAL "works" OR NOT err STREQUAL "")
    message(FATAL_ERROR "Relocated CLI persistence failed (${status}):\n${out}\n${err}")
endif()
run_checked("${CMAKE_COMMAND}" -S "${POCKET_SOURCE_DIR}/tests/consumer" -B "${consumer_build}"
    "-DCMAKE_PREFIX_PATH=${relocated_prefix}"
    "-DPocketKV_DIR=${relocated_prefix}/${POCKET_LIBDIR}/cmake/PocketKV"
    "-DCMAKE_CXX_COMPILER=${POCKET_COMPILER}" "-DCMAKE_BUILD_TYPE=${POCKET_CONFIG}")
run_checked("${CMAKE_COMMAND}" --build "${consumer_build}" ${config_args})
run_checked("${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build}" ${ctest_config_args} --output-on-failure)
