cmake_minimum_required(VERSION 3.24)

project(BiblioLED VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# SQLite 3.54.0 and later require Windows SRW locks in the default Windows
# implementation. Those APIs are unavailable on Windows XP. The 3.53.4
# amalgamation is therefore pinned for the XP proof of concept.
if(WIN32)
    enable_language(RC)

    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "BiblioLED is currently a Win32 (32-bit) application.")
    endif()

    include(FetchContent)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Declare(sqlite_amalgamation
        URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
        URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
    )
    FetchContent_GetProperties(sqlite_amalgamation)
    if(NOT sqlite_amalgamation_POPULATED)
        FetchContent_Populate(sqlite_amalgamation)
    endif()

    set(SQLITE_AMALGAMATION_DIR "${sqlite_amalgamation_SOURCE_DIR}")
    if(NOT EXISTS "${SQLITE_AMALGAMATION_DIR}/sqlite3.c")
        set(SQLITE_AMALGAMATION_DIR "${sqlite_amalgamation_SOURCE_DIR}/sqlite-amalgamation-3530400")
    endif()
    if(NOT EXISTS "${SQLITE_AMALGAMATION_DIR}/sqlite3.c")
        message(FATAL_ERROR "Could not locate sqlite3.c in the downloaded SQLite amalgamation.")
    endif()

    add_library(sqlite3 STATIC "${SQLITE_AMALGAMATION_DIR}/sqlite3.c")
    target_include_directories(sqlite3 PUBLIC "${SQLITE_AMALGAMATION_DIR}")
    target_compile_definitions(sqlite3 PRIVATE
        SQLITE_THREADSAFE=1
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_DEFAULT_FOREIGN_KEYS=1
        SQLITE_DQS=0
    )
else()
    find_package(SQLite3 REQUIRED)
endif()

add_library(biblioled_core
    src/core/application_paths.cpp
    src/core/csv_export.cpp
    src/core/loan_rules.cpp
    src/core/loan_statistics.cpp
    src/core/loan_store.cpp
    src/infrastructure/settings_store.cpp
    src/infrastructure/backup_compression.cpp
    src/infrastructure/backup_retention.cpp
)
target_include_directories(biblioled_core PUBLIC include)

if(WIN32)
    # zlib is built statically; only its stream API is used by the backup layer.
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_MINIZIP OFF CACHE BOOL "" FORCE)
    set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.4/zlib-1.4.tar.gz
        URL_HASH SHA256=02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584
    )
    FetchContent_MakeAvailable(zlib)

    target_link_libraries(biblioled_core PUBLIC sqlite3 zlibstatic)
    target_compile_definitions(biblioled_core PRIVATE
        WINVER=0x0501
        _WIN32_WINNT=0x0501
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    add_executable(BiblioLED WIN32
        src/win32/main.cpp
        src/win32/history_dialog.cpp
        src/win32/statistics_dialog.cpp
        src/win32/dialogs.rc
        src/infrastructure/audit_log.cpp
    )
    target_include_directories(BiblioLED PRIVATE src/win32)
    target_compile_options(BiblioLED PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    target_compile_definitions(BiblioLED PRIVATE
        WINVER=0x0501
        _WIN32_WINNT=0x0501
        _WIN32_IE=0x0600
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        BIBLIOLED_VERSION="${PROJECT_VERSION}"
    )
    target_link_libraries(BiblioLED PRIVATE biblioled_core comctl32 comdlg32)

    if(MSVC)
        target_link_options(BiblioLED PRIVATE "/SUBSYSTEM:WINDOWS,5.01" "/MANIFEST:NO")
    elseif(MINGW)
        # wWinMain requires the Unicode CRT entry point in MinGW.
        target_link_options(BiblioLED PRIVATE "-municode" "-Wl,--subsystem,windows:5.01")
    endif()

    add_custom_command(TARGET BiblioLED POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:BiblioLED>/config"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config/settings.ini"
            "$<TARGET_FILE_DIR:BiblioLED>/config/settings.ini"
        VERBATIM
    )
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(biblioled_core PUBLIC SQLite::SQLite3 ZLIB::ZLIB)
endif()

if(MSVC)
    target_compile_options(biblioled_core PRIVATE /W4)
else()
    target_compile_options(biblioled_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(application_paths_tests tests/application_paths_tests.cpp)
    target_link_libraries(application_paths_tests PRIVATE biblioled_core)
    add_test(NAME application_paths_tests COMMAND application_paths_tests)

    add_executable(settings_store_tests tests/settings_store_tests.cpp)
    target_link_libraries(settings_store_tests PRIVATE biblioled_core)
    add_test(NAME settings_store_tests COMMAND settings_store_tests)

    add_executable(loan_csv_export_tests tests/loan_csv_export_tests.cpp)
    target_link_libraries(loan_csv_export_tests PRIVATE biblioled_core)
    add_test(NAME loan_csv_export_tests COMMAND loan_csv_export_tests)

    add_executable(loan_rules_tests tests/loan_rules_tests.cpp)
    target_link_libraries(loan_rules_tests PRIVATE biblioled_core)
    add_test(NAME loan_rules_tests COMMAND loan_rules_tests)

    add_executable(loan_statistics_tests tests/loan_statistics_tests.cpp)
    target_link_libraries(loan_statistics_tests PRIVATE biblioled_core)
    add_test(NAME loan_statistics_tests COMMAND loan_statistics_tests)

    add_executable(loan_store_tests tests/loan_store_tests.cpp)
    target_link_libraries(loan_store_tests PRIVATE biblioled_core)
    add_test(NAME loan_store_tests COMMAND loan_store_tests)

    add_executable(backup_compression_tests tests/backup_compression_tests.cpp)
    target_link_libraries(backup_compression_tests PRIVATE biblioled_core)
    add_test(NAME backup_compression_tests COMMAND backup_compression_tests)

    add_executable(backup_retention_tests tests/backup_retention_tests.cpp)
    target_link_libraries(backup_retention_tests PRIVATE biblioled_core)
    add_test(NAME backup_retention_tests COMMAND backup_retention_tests)

    if(WIN32)
        add_executable(audit_log_tests
            tests/audit_log_tests.cpp
            src/infrastructure/audit_log.cpp
        )
        target_include_directories(audit_log_tests PRIVATE include)
        target_compile_definitions(audit_log_tests PRIVATE
            WINVER=0x0501
            _WIN32_WINNT=0x0501
            WIN32_LEAN_AND_MEAN
            NOMINMAX
        )
        add_test(NAME audit_log_tests COMMAND audit_log_tests)
    endif()
endif()
