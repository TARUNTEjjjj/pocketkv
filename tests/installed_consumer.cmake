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
# Configure the consumer the way the README tells users to. Given only a prefix,
# find_package always searches <prefix>/lib/cmake/PocketKV, but other library
# directories only on some platforms (Debian and Ubuntu skip lib64) or never
# (tools/lib), so the README adds PocketKV_DIR for those.
set(package_dir "${relocated_prefix}/${POCKET_LIBDIR}/cmake/PocketKV")
set(discovery "-DCMAKE_PREFIX_PATH=${relocated_prefix}")
if(NOT POCKET_LIBDIR STREQUAL "lib")
    list(APPEND discovery "-DPocketKV_DIR=${package_dir}")
endif()
run_checked("${CMAKE_COMMAND}" -S "${POCKET_SOURCE_DIR}/tests/consumer" -B "${consumer_build}"
    ${discovery}
    "-DCMAKE_CXX_COMPILER=${POCKET_COMPILER}" "-DCMAKE_BUILD_TYPE=${POCKET_CONFIG}")
# The package must come from the moved installation, not from another PocketKV
# that the search reached first.
file(STRINGS "${consumer_build}/CMakeCache.txt" found REGEX "^PocketKV_DIR:[A-Z]+=")
list(TRANSFORM found REPLACE "^PocketKV_DIR:[A-Z]+=" "")
if(NOT found STREQUAL "")
    file(REAL_PATH "${found}" found)
endif()
file(REAL_PATH "${package_dir}" expected)
if(NOT found STREQUAL expected)
    message(FATAL_ERROR "Consumer used PocketKV from '${found}', not the moved installation '${expected}'")
endif()
run_checked("${CMAKE_COMMAND}" --build "${consumer_build}" ${config_args})
run_checked("${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build}" ${ctest_config_args} --output-on-failure)
