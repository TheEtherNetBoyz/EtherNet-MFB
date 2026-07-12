cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(tpcm_core LANGUAGES CXX)
endif()

if(NOT TARGET zlibstatic AND NOT TARGET zlibstaticd AND NOT TARGET ZLIB::ZLIB AND NOT TARGET zlib)
    find_package(ZLIB QUIET)
endif()
if(NOT TARGET zlibstatic AND NOT TARGET zlibstaticd AND NOT TARGET ZLIB::ZLIB AND NOT TARGET zlib)
    include(FetchContent)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(zlib)
endif()

add_library(tpcm_core STATIC
    src/BgmDatabase.cpp
    src/BmsConverter.cpp
    src/InstrumentMap.cpp
    src/InstrumentMapData.cpp
    src/IsoFileSystem.cpp
    src/ManifestBuilder.cpp
    src/MusicRandomizer.cpp
    src/RarcInjector.cpp
    src/TprsReader.cpp
    src/WaveBankBuilder.cpp
)

target_include_directories(tpcm_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(tpcm_core PUBLIC cxx_std_17)
target_compile_options(tpcm_core PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

if(TARGET zlibstatic)
    target_link_libraries(tpcm_core PUBLIC zlibstatic)
elseif(TARGET zlibstaticd)
    target_link_libraries(tpcm_core PUBLIC zlibstaticd)
elseif(TARGET ZLIB::ZLIB)
    target_link_libraries(tpcm_core PUBLIC ZLIB::ZLIB)
elseif(TARGET zlib)
    target_link_libraries(tpcm_core PUBLIC zlib)
else()
    message(FATAL_ERROR "tpcm_core requires zlib for normal compressed TPRS archives")
endif()
target_compile_definitions(tpcm_core PUBLIC TPCM_HAS_ZLIB)

option(TPCM_BUILD_TESTS "Build tpcm_core tests" ON)

if(TPCM_BUILD_TESTS)
    enable_testing()

    add_executable(tpcm_bgm_database_test
        tests/BgmDatabaseTest.cpp
    )
    target_link_libraries(tpcm_bgm_database_test PRIVATE tpcm_core)

    add_test(NAME tpcm_bgm_database_test COMMAND tpcm_bgm_database_test)

    add_executable(tpcm_music_randomizer_test
        tests/MusicRandomizerTest.cpp
    )
    target_link_libraries(tpcm_music_randomizer_test PRIVATE tpcm_core)

    add_test(NAME tpcm_music_randomizer_test COMMAND tpcm_music_randomizer_test)

    add_executable(tpcm_tprs_reader_test
        tests/TprsReaderTest.cpp
    )
    target_link_libraries(tpcm_tprs_reader_test PRIVATE tpcm_core)

    add_test(NAME tpcm_tprs_reader_test COMMAND tpcm_tprs_reader_test)

    add_executable(tpcm_manifest_builder_test
        tests/ManifestBuilderTest.cpp
    )
    target_link_libraries(tpcm_manifest_builder_test PRIVATE tpcm_core)

    add_test(NAME tpcm_manifest_builder_test COMMAND tpcm_manifest_builder_test)

    add_executable(tpcm_bms_converter_test
        tests/BmsConverterTest.cpp
    )
    target_link_libraries(tpcm_bms_converter_test PRIVATE tpcm_core)

    add_test(NAME tpcm_bms_converter_test COMMAND tpcm_bms_converter_test)

    add_executable(tpcm_rarc_injector_test
        tests/RarcInjectorTest.cpp
    )
    target_link_libraries(tpcm_rarc_injector_test PRIVATE tpcm_core)

    add_test(NAME tpcm_rarc_injector_test COMMAND tpcm_rarc_injector_test)

    add_executable(tpcm_wave_bank_builder_test
        tests/WaveBankBuilderTest.cpp
    )
    target_link_libraries(tpcm_wave_bank_builder_test PRIVATE tpcm_core)
    target_compile_features(tpcm_wave_bank_builder_test PRIVATE cxx_std_17)

    add_test(NAME tpcm_wave_bank_builder_test COMMAND tpcm_wave_bank_builder_test)

    add_executable(tpcm_instrument_map_test
        tests/InstrumentMapTest.cpp
    )
    target_link_libraries(tpcm_instrument_map_test PRIVATE tpcm_core)

    add_test(NAME tpcm_instrument_map_test COMMAND tpcm_instrument_map_test)

    add_executable(tpcm_iso_fs_test
        tests/IsoFileSystemTest.cpp
    )
    target_link_libraries(tpcm_iso_fs_test PRIVATE tpcm_core)
    target_compile_features(tpcm_iso_fs_test PRIVATE cxx_std_17)

    add_test(NAME tpcm_iso_fs_test COMMAND tpcm_iso_fs_test)
endif()
