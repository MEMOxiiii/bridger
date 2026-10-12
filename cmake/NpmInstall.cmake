# cmake -DNPM=<npm> -DDIR=<package dir> -P NpmInstall.cmake
#
# Runs `npm ci` only when node_modules is missing or older than package-lock.json, so several
# build presets can share one node_modules without reinstalling it each time.
set(lock "${DIR}/package-lock.json")
set(installed "${DIR}/node_modules/.package-lock.json")
if(EXISTS "${installed}" AND NOT "${lock}" IS_NEWER_THAN "${installed}")
    return()
endif()
execute_process(COMMAND "${NPM}" ci --no-audit --no-fund WORKING_DIRECTORY "${DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "npm ci failed (${result})")
endif()
