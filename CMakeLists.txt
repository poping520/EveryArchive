cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(EveryZip LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Disable zlib example targets" FORCE)
add_subdirectory(external/zlib)

add_library(ezdb STATIC
        src/ezdb/ezdb.c
        src/ezdb/ezdb_build.c
        src/ezdb/ezdb_delta.c
        src/ezdb/ezdb_entries.c
        src/ezdb/ezdb_format.c
        src/ezdb/ezdb_io.c
        src/ezdb/ezdb_postings.c
        src/ezdb/ezdb_query.c
        src/ezdb/ezdb.h
        src/ezdb/ezdb_build.h
        src/ezdb/ezdb_core_internal.h
        src/ezdb/ezdb_delta.h
        src/ezdb/ezdb_entries.h
        src/ezdb/ezdb_format.h
        src/ezdb/ezdb_internal.h
        src/ezdb/ezdb_io.h
        src/ezdb/ezdb_postings.h
        src/ezdb/ezdb_query.h
)

target_include_directories(ezdb PUBLIC
        src/ezdb
)

target_include_directories(ezdb PRIVATE
        external/zlib
)

target_link_libraries(ezdb PUBLIC zlibstatic)

if(MSVC)
    target_compile_options(ezdb PRIVATE /utf-8)
endif()

add_library(edb STATIC
        src/edb/edb.c
        src/edb/edb_build.c
        src/edb/edb_format.c
        src/edb/edb_postings.c
        src/edb/edb_search.c
        src/edb/edb_util.c
)

target_include_directories(edb PUBLIC
        src/edb
)

target_include_directories(edb PRIVATE
        external/zlib
)

target_link_libraries(edb PUBLIC zlibstatic)

if(MSVC)
    target_compile_options(edb PRIVATE /utf-8)
endif()

add_executable(EveryZip WIN32
        src/resource.h
        src/strings.rc
        src/main.cpp
        src/window/main_window.h
        src/window/main_window.cpp
        src/window/about_window.h
        src/window/about_window.cpp
        src/window/settings_window.h
        src/window/settings_window.cpp
        src/window/update_prompt_window.h
        src/window/update_prompt_window.cpp
        src/window/window_utils.h
        src/window/window_utils.cpp
        src/string_utils.h
        src/string_utils.cpp
        src/window/license_text.h
        src/window/license_text.cpp
        src/tray_icon.h
        src/tray_icon.cpp
        src/icon_cache.h
        src/icon_cache.cpp
        src/row_cache.h
        src/row_cache.cpp
        src/indexer.h
        src/indexer.cpp
        src/types.h
        src/file_scanner.h
        src/file_scanner.cpp
 
        src/index_store.h
        src/index_store.cpp
        src/database.h
        src/database.cpp
        src/logger.cpp
        src/app_version.h
        src/update_checker.h
        src/update_checker.cpp
        
        src/config/advconfig.h
        src/config/advconfig.cpp
        src/config/user_config.h
        src/config/user_config.cpp

        src/parser/archive_parser.h
        src/parser/archive_parser_factory.h
        src/parser/archive_parser_factory.cpp
        src/parser/zip_archive_parser.h
        src/parser/zip_archive_parser.cpp
        src/parser/zip_cd_scanner.c
        src/parser/zip_cd_scanner.h
        src/parser/sevenzip_archive_parser.h
        src/parser/sevenzip_archive_parser.cpp
        src/parser/rar_archive_parser.h
        src/parser/rar_archive_parser.cpp
        # src/parser/libarchive_parser.h
        # src/parser/libarchive_parser.cpp

        external/sqlite/sqlite3.c
)

add_library(minizip STATIC
        external/zlib/contrib/minizip/ioapi.c
        external/zlib/contrib/minizip/iowin32.c
        external/zlib/contrib/minizip/mztools.c
        external/zlib/contrib/minizip/unzip.c
)

target_include_directories(minizip PUBLIC
        external/zlib
        external/zlib/contrib/minizip
)

target_link_libraries(minizip PUBLIC zlibstatic)

add_subdirectory(tools)

add_library(sevenzip_c STATIC
        external/7zip/C/7zAlloc.c
        external/7zip/C/7zBuf.c
        external/7zip/C/7zFile.c
        external/7zip/C/7zDec.c
        external/7zip/C/7zArcIn.c
        external/7zip/C/7zStream.c
        external/7zip/C/Bcj2.c
        external/7zip/C/Bra.c
        external/7zip/C/Bra86.c
        external/7zip/C/BraIA64.c
        external/7zip/C/CpuArch.c
        external/7zip/C/Delta.c
        external/7zip/C/Lzma2Dec.c
        external/7zip/C/LzmaDec.c
        external/7zip/C/Ppmd7.c
        external/7zip/C/Ppmd7Dec.c
        external/7zip/C/7zCrc.c
        external/7zip/C/7zCrcOpt.c
)

target_include_directories(sevenzip_c PUBLIC
        external/7zip/C
)

target_compile_definitions(sevenzip_c PUBLIC
        Z7_EXTRACT_ONLY
        Z7_PPMD_SUPPORT
        USE_C_CRC
)

add_library(unrar_static STATIC
        external/unrar/rar.cpp
        external/unrar/strlist.cpp
        external/unrar/strfn.cpp
        external/unrar/pathfn.cpp
        external/unrar/smallfn.cpp
        external/unrar/global.cpp
        external/unrar/file.cpp
        external/unrar/filefn.cpp
        external/unrar/filcreat.cpp
        external/unrar/archive.cpp
        external/unrar/arcread.cpp
        external/unrar/unicode.cpp
        external/unrar/system.cpp
        external/unrar/crypt.cpp
        external/unrar/crc.cpp
        external/unrar/rawread.cpp
        external/unrar/encname.cpp
        external/unrar/resource.cpp
        external/unrar/match.cpp
        external/unrar/timefn.cpp
        external/unrar/rdwrfn.cpp
        external/unrar/consio.cpp
        external/unrar/options.cpp
        external/unrar/errhnd.cpp
        external/unrar/rarvm.cpp
        external/unrar/secpassword.cpp
        external/unrar/rijndael.cpp
        external/unrar/getbits.cpp
        external/unrar/sha1.cpp
        external/unrar/sha256.cpp
        external/unrar/blake2s.cpp
        external/unrar/hash.cpp
        external/unrar/extinfo.cpp
        external/unrar/extract.cpp
        external/unrar/volume.cpp
        external/unrar/list.cpp
        external/unrar/find.cpp
        external/unrar/unpack.cpp
        external/unrar/headers.cpp
        external/unrar/threadpool.cpp
        external/unrar/rs16.cpp
        external/unrar/cmddata.cpp
        external/unrar/ui.cpp
        external/unrar/largepage.cpp
        external/unrar/isnt.cpp
        external/unrar/motw.cpp
        external/unrar/filestr.cpp
        external/unrar/scantree.cpp
        external/unrar/dll.cpp
        external/unrar/qopen.cpp
)

target_include_directories(unrar_static PUBLIC
        external/unrar
)

target_compile_definitions(unrar_static PRIVATE
        RARDLL
        UNICODE
        _UNICODE
)

if(MSVC)
    target_compile_options(unrar_static PRIVATE /utf-8)
endif()

target_link_libraries(unrar_static PUBLIC
        shlwapi
        powrprof
        psapi
        wbemuuid
)

target_include_directories(EveryZip PRIVATE
        src/ezdb
        external/sqlite
        external/zlib
        external/zlib/contrib/minizip
        external/7zip/C
        external/unrar
)

target_compile_definitions(EveryZip PRIVATE UNICODE _UNICODE SQLITE_ENABLE_FTS5)

if(MSVC)
    target_compile_options(EveryZip PRIVATE /utf-8)
endif()

if(MSVC)
    # manifest 已通过 RC 文件嵌入（strings.rc: 1 RT_MANIFEST "app.manifest"），
    # 禁止链接器自动生成并合并 manifest，避免覆盖 DPI 感知声明
    target_link_options(EveryZip PRIVATE /MANIFEST:NO)
endif()

target_link_libraries(EveryZip PRIVATE
        ezdb
        comctl32
        shlwapi
        winhttp
        minizip
        sevenzip_c
        unrar_static
)

enable_testing()
add_subdirectory(test)
