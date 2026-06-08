cmake_minimum_required(VERSION 3.20)

project(EveryZip LANGUAGES C CXX)

enable_testing()

find_package(Python3 COMPONENTS Interpreter QUIET)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(everyzip_configure_test_target target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /utf-8)
    endif()
endfunction()

add_executable(TestZip
        test_zip.cpp
        ../src/parser/archive_parser.h
        ../src/parser/zip_archive_parser.h
        ../src/parser/zip_archive_parser.cpp
        ../src/parser/zip_cd_scanner.c
        ../src/parser/zip_cd_scanner.h
        ../src/string_utils.cpp
)

add_executable(TestSevenZip
        test_sevenzip.cpp
        ../src/parser/archive_parser.h
        ../src/parser/sevenzip_archive_parser.h
        ../src/parser/sevenzip_archive_parser.cpp
        ../src/string_utils.cpp
)

add_executable(TestRar
        test_rar.cpp
        ../src/parser/archive_parser.h
        ../src/parser/rar_archive_parser.h
        ../src/parser/rar_archive_parser.cpp
        ../src/string_utils.cpp
)

add_executable(TestUsnScan
        test_usn_scan.cpp
)

add_executable(TestMftScan
        test_mft_scan.cpp
)

add_executable(TestFdb
        test_fdb.c
)

target_link_libraries(TestFdb PRIVATE
        psapi
)

add_executable(TestFileTracker
        test_file_tracker.cpp
        ../external/sqlite/sqlite3.c
)

add_executable(TestIndexStore
        test_index_store.cpp
        ../src/index_store.cpp
        ../src/database.cpp
        ../src/string_utils.cpp
        ../src/logger.cpp
        ../external/sqlite/sqlite3.c
)

add_executable(QueryDB
        query_db.cpp
        ../external/sqlite/sqlite3.c
)

target_link_libraries(TestZip PRIVATE
        minizip
        shlwapi
)

target_include_directories(TestZip PRIVATE
        ../src
        ../external/zlib
        ../external/zlib/contrib/minizip
)

target_link_libraries(TestSevenZip PRIVATE
        sevenzip_c
        shlwapi
)

target_include_directories(TestSevenZip PRIVATE
        ../src
        ../external/7zip/C
)

target_link_libraries(TestRar PRIVATE
        unrar_static
)

target_include_directories(TestRar PRIVATE
        ../src
        ../external/unrar
)

target_include_directories(TestFileTracker PRIVATE
        ../external/sqlite
)

target_include_directories(TestIndexStore PRIVATE
        ../src
        ../src/ezdb
        ../external/sqlite
)

target_link_libraries(TestIndexStore PRIVATE
        ezdb
)

target_include_directories(QueryDB PRIVATE
        ../external/sqlite
)

add_executable(TestConfig
        test_config.cpp
        ../src/config/user_config.cpp
        ../src/config/advconfig.cpp
        ../src/string_utils.cpp
        ../src/logger.cpp
)

target_include_directories(TestConfig PRIVATE
        ../src/config
)

target_link_libraries(TestConfig PRIVATE
        shlwapi
)

target_compile_definitions(TestConfig PRIVATE UNICODE _UNICODE)

target_compile_definitions(TestSevenZip PRIVATE UNICODE _UNICODE)
target_compile_definitions(TestRar PRIVATE UNICODE _UNICODE)
target_compile_definitions(TestFileTracker PRIVATE UNICODE _UNICODE)
target_compile_definitions(TestIndexStore PRIVATE UNICODE _UNICODE)

everyzip_configure_test_target(TestZip)
everyzip_configure_test_target(TestSevenZip)
everyzip_configure_test_target(TestRar)
everyzip_configure_test_target(TestUsnScan)
everyzip_configure_test_target(TestMftScan)
everyzip_configure_test_target(TestFdb)
everyzip_configure_test_target(TestFileTracker)
everyzip_configure_test_target(TestIndexStore)
everyzip_configure_test_target(QueryDB)
everyzip_configure_test_target(TestConfig)

add_test(NAME TestIndexStore COMMAND TestIndexStore)

if(Python3_Interpreter_FOUND)
    add_test(NAME EzdbZipFixtureTests
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/ezdb_zip_fixture_tests.py
                    --bench $<TARGET_FILE:EzdbBench>
                    --work-dir ${CMAKE_BINARY_DIR}/ezdb_zip_fixture_tests)
endif()
