add_executable(EzdbBench
        ezdb_bench.c
)

target_link_libraries(EzdbBench PRIVATE ezdb psapi)

if(MSVC)
    target_compile_options(EzdbBench PRIVATE /utf-8)
endif()

add_executable(ListAllFiles
        list_all_files.c
)

if(MSVC)
    target_compile_options(ListAllFiles PRIVATE /utf-8)
endif()
