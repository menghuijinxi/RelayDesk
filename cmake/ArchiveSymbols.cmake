# 归档一次构建的二进制与符号，供崩溃转储事后符号化使用。
#
# 崩溃报告刻意不携带 pdb：它体积远超转储本身，不适合随诊断包回传。取而代之的是
# 把每次发布的 exe/pdb 按版本集中归档，排查时用归档里的 pdb 还原偏移量。
#
# 调用方式（由 CMakeLists.txt 的 POST_BUILD 触发）：
#   cmake -DSYMBOL_TARGET_FILE=<exe> -DSYMBOL_TARGET_PDB=<pdb>
#         -DSYMBOL_CONFIG=<配置> -DSYMBOL_ARCHIVE_DIR=<目录>
#         -P cmake/ArchiveSymbols.cmake

if(NOT SYMBOL_TARGET_FILE)
    message(FATAL_ERROR "SYMBOL_TARGET_FILE is required.")
endif()
if(NOT SYMBOL_ARCHIVE_DIR)
    message(FATAL_ERROR "SYMBOL_ARCHIVE_DIR is required.")
endif()

get_filename_component(SYMBOL_EXECUTABLE_NAME "${SYMBOL_TARGET_FILE}" NAME)
get_filename_component(SYMBOL_EXECUTABLE_STEM "${SYMBOL_TARGET_FILE}" NAME_WE)
get_filename_component(SYMBOL_ARCHIVE_ROOT "${SYMBOL_ARCHIVE_DIR}" ABSOLUTE)

# 版本号是崩溃报告和符号归档之间的唯一关联键，必须来自同一个源文件。
set(SYMBOL_APP_VERSION_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../src/core/app_version.cpp")
if(EXISTS "${SYMBOL_APP_VERSION_FILE}")
    file(READ "${SYMBOL_APP_VERSION_FILE}" SYMBOL_APP_VERSION_SOURCE)
    if(SYMBOL_APP_VERSION_SOURCE MATCHES
       "kAppVersion[ \t]*=[ \t]*([0-9]+)")
        set(SYMBOL_APP_VERSION "${CMAKE_MATCH_1}")
    else()
        set(SYMBOL_APP_VERSION "unknown")
    endif()
else()
    set(SYMBOL_APP_VERSION "unknown")
endif()

set(SYMBOL_DESTINATION
    "${SYMBOL_ARCHIVE_ROOT}/v${SYMBOL_APP_VERSION}-${SYMBOL_CONFIG}")
file(MAKE_DIRECTORY "${SYMBOL_DESTINATION}")

file(SHA256 "${SYMBOL_TARGET_FILE}" SYMBOL_EXECUTABLE_SHA256)
file(COPY_FILE "${SYMBOL_TARGET_FILE}"
     "${SYMBOL_DESTINATION}/${SYMBOL_EXECUTABLE_NAME}" ONLY_IF_DIFFERENT)

set(SYMBOL_PDB_STATUS "missing")
if(SYMBOL_TARGET_PDB AND EXISTS "${SYMBOL_TARGET_PDB}")
    get_filename_component(SYMBOL_PDB_NAME "${SYMBOL_TARGET_PDB}" NAME)
    file(COPY_FILE "${SYMBOL_TARGET_PDB}"
         "${SYMBOL_DESTINATION}/${SYMBOL_PDB_NAME}" ONLY_IF_DIFFERENT)
    set(SYMBOL_PDB_STATUS "archived")
elseif(NOT SYMBOL_TARGET_PDB)
    set(SYMBOL_PDB_STATUS "not_produced")
endif()

string(TIMESTAMP SYMBOL_ARCHIVED_AT "%Y-%m-%dT%H:%M:%SZ" UTC)

set(SYMBOL_MANIFEST
    "archived_at=${SYMBOL_ARCHIVED_AT}\n\
app_version=${SYMBOL_APP_VERSION}\n\
build_configuration=${SYMBOL_CONFIG}\n\
executable=${SYMBOL_EXECUTABLE_NAME}\n\
executable_sha256=${SYMBOL_EXECUTABLE_SHA256}\n\
pdb_status=${SYMBOL_PDB_STATUS}\n\
usage=用本目录下的 pdb 对同名版本的 crash.dmp 做符号化\n")
file(WRITE "${SYMBOL_DESTINATION}/manifest.txt" "${SYMBOL_MANIFEST}")

message(STATUS
    "Archived symbols: ${SYMBOL_DESTINATION} (pdb: ${SYMBOL_PDB_STATUS})")
