cmake_minimum_required(VERSION 3.20)
project(TVTestHTTPPlugin LANGUAGES CXX)

# Windows x64 専用
if(NOT WIN32)
    message(FATAL_ERROR "Windows 専用プロジェクトです")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "x64 (64bit) でビルドしてください")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# 依存ヘッダーのダウンロード (初回ビルド時のみ)
# =============================================================================

set(SDK_DIR "${CMAKE_CURRENT_BINARY_DIR}/sdk")
file(MAKE_DIRECTORY "${SDK_DIR}")

# TVTest Plugin SDK
set(TVTEST_PLUGIN_H "${SDK_DIR}/TVTestPlugin.h")
if(NOT EXISTS "${TVTEST_PLUGIN_H}")
    message(STATUS "Downloading TVTestPlugin.h ...")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/DBCTRADO/TVTest/master/sdk/TVTestPlugin.h"
        "${TVTEST_PLUGIN_H}"
        SHOW_PROGRESS
        STATUS DL_STATUS
    )
    list(GET DL_STATUS 0 DL_CODE)
    if(NOT DL_CODE EQUAL 0)
        message(FATAL_ERROR "TVTestPlugin.h のダウンロードに失敗しました (${DL_STATUS})")
    endif()
endif()

# cpp-httplib (ヘッダーオンリー HTTP サーバー)
set(HTTPLIB_H "${SDK_DIR}/httplib.h")
if(NOT EXISTS "${HTTPLIB_H}")
    message(STATUS "Downloading httplib.h ...")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h"
        "${HTTPLIB_H}"
        SHOW_PROGRESS
        STATUS DL_STATUS
    )
    list(GET DL_STATUS 0 DL_CODE)
    if(NOT DL_CODE EQUAL 0)
        message(FATAL_ERROR "httplib.h のダウンロードに失敗しました (${DL_STATUS})")
    endif()
endif()

# =============================================================================
# ターゲット
# =============================================================================

add_library(TVTestHTTPPlugin SHARED TVTestHTTPPlugin.cpp TVTestHTTPPlugin.rc)

target_include_directories(TVTestHTTPPlugin PRIVATE "${SDK_DIR}")

target_compile_definitions(TVTestHTTPPlugin PRIVATE
    # CPPHTTPLIB_OPENSSL_SUPPORT は定義しない = OpenSSL 無効 (#ifdef で判定されるため)
    # NOMINMAX は定義しない: TVTestPlugin.h が windows.h の min/max マクロを必要とするため
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

target_compile_options(TVTestHTTPPlugin PRIVATE
    /W3         # 警告レベル 3
    /utf-8      # ソースを UTF-8 として扱う
    /MP         # 並列コンパイル
)

# VC++ ランタイムをスタティックリンク (配布時に Redist 不要)
set_property(TARGET TVTestHTTPPlugin PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# 出力ファイルを .tvtp に
set_target_properties(TVTestHTTPPlugin PROPERTIES
    OUTPUT_NAME "TVTestHTTPPlugin"
    SUFFIX      ".tvtp"
)

# ビルド後に Plugins フォルダへコピー (TVTest_64bit のパスは環境に合わせて変更)
# set(TVTEST_PLUGINS_DIR "C:/TVTest/Plugins")
# add_custom_command(TARGET TVTestHTTPPlugin POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:TVTestHTTPPlugin> "${TVTEST_PLUGINS_DIR}/"
#     COMMENT "Copying to TVTest Plugins folder"
# )
