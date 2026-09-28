# macOS 建置：共用核心（core/）、鼠鬚管的橋接層、LLM 推理程式與測試。
#
#   cmake -S mac -B build -DRIME_ROOT=<librime 的安裝前綴> [-DLLAMA_ROOT=<llama.cpp 的安裝前綴>]
#   cmake --build build && ctest --test-dir build --output-on-failure
#
# 沒有 RIME_ROOT 時用 Homebrew 的 librime（brew install librime）。
# LLAMA_ROOT 用 mac/get-llama-runtime.sh 下載的 output/llama-mac；沒有時不建推理程式（WisdomLLMHost），其他照常。
cmake_minimum_required(VERSION 3.20)
# 與鼠鬚管相同（13.0）；要在 project() 之前設定才有作用，鼠鬚管的 Makefile 會傳入它的版本
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "")
project(WisdomWeaselMac LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(CORE "${ROOT}/core")

# ---- 相依套件
if(NOT RIME_ROOT)
  execute_process(COMMAND brew --prefix librime OUTPUT_VARIABLE RIME_ROOT
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
find_library(RIME_LIB rime HINTS "${RIME_ROOT}/lib")
find_package(CURL REQUIRED)
find_library(SECURITY Security REQUIRED)
find_library(COREFOUNDATION CoreFoundation REQUIRED)
find_library(APPKIT AppKit REQUIRED)
find_library(WEBKIT WebKit REQUIRED)
find_library(UTTYPES UniformTypeIdentifiers REQUIRED)

# rime_api.h 用倉庫裡的（與小狼毫相同版本）；Homebrew 的在後面備用
set(COMMON_INCLUDES "${ROOT}/include" "${RIME_ROOT}/include")

# ---- 共用核心（輸入法端）
add_library(wisdom_core STATIC
  ${CORE}/crypto/personal_crypto.cpp
  ${CORE}/platform/mac/key_store_mac.cpp
  ${CORE}/platform/posix/process_posix.cpp
  ${CORE}/net/curl/http_curl.cpp
  ${CORE}/llm/LLMProvider.cpp
  ${CORE}/llm/RemoteLLMProvider.cpp
  ${CORE}/llm/ContextHistory.cpp
  ${CORE}/llm/MemoryCompressor.cpp
  ${CORE}/llm_ipc/client.cpp
  ${CORE}/personal/PersonalLexicon.cpp
  ${CORE}/personal/PersonalRefiner.cpp
  ${CORE}/ime/controller.cpp
  ${CORE}/ime/prediction_engine.cpp
  ${CORE}/ime/rescore.cpp
  ${CORE}/ime/text_rules.cpp
  ${CORE}/ime/choice_stats.cpp
  ${CORE}/ime/choice_log.cpp
  ${CORE}/ime/rime_helpers.cpp
  ${CORE}/settings/backend.cpp
  ${CORE}/settings/ops.cpp
  ${CORE}/third_party/monocypher/monocypher.c
)
target_include_directories(wisdom_core PUBLIC ${COMMON_INCLUDES})
target_link_libraries(wisdom_core PUBLIC CURL::libcurl ${SECURITY} ${COREFOUNDATION})
if(RIME_LIB)
  target_link_libraries(wisdom_core PUBLIC ${RIME_LIB})
endif()

# ---- 鼠鬚管的橋接層與網頁版設定視窗（鼠鬚管的 Xcode 專案連結這個）
add_library(wisdom_squirrel STATIC
  ${ROOT}/mac/squirrel/WisdomBridge.mm
  ${ROOT}/mac/settings/MacSettingsPlatform.mm
  ${ROOT}/mac/settings/WisdomSettingsWindow.mm
)
target_compile_options(wisdom_squirrel PRIVATE -fobjc-arc)
target_link_libraries(wisdom_squirrel PUBLIC wisdom_core ${APPKIT} ${WEBKIT} ${UTTYPES})

# ---- LLM 推理程式（放進 Squirrel.app/Contents/MacOS）
if(LLAMA_ROOT AND RIME_LIB)
  find_library(LLAMA_LIB llama HINTS "${LLAMA_ROOT}/lib" REQUIRED)
  find_library(GGML_LIB ggml HINTS "${LLAMA_ROOT}/lib" REQUIRED)
  find_library(GGML_BASE_LIB ggml-base HINTS "${LLAMA_ROOT}/lib")
  # llama.h 引用 ggml.h；Homebrew 的 ggml 是另一個套件，不在 llama.cpp 的前綴裡
  find_path(GGML_INCLUDE ggml.h HINTS "${LLAMA_ROOT}/include" REQUIRED)
  add_executable(WisdomLLMHost
    ${ROOT}/mac/WisdomLLMHost/main.cpp
    ${CORE}/llm/host_service.cpp
    ${CORE}/llm/LLMProvider.cpp
    ${CORE}/llm/LlamaCppProvider.cpp
    ${CORE}/llm/HFConstraintProvider.cpp
    ${CORE}/llm_ipc/host.cpp
    ${CORE}/platform/posix/process_posix.cpp
    ${CORE}/net/curl/http_curl.cpp
  )
  target_include_directories(WisdomLLMHost PRIVATE ${COMMON_INCLUDES} "${LLAMA_ROOT}/include" "${GGML_INCLUDE}")
  target_link_libraries(WisdomLLMHost PRIVATE ${LLAMA_LIB} ${GGML_LIB} CURL::libcurl ${RIME_LIB})
  if(GGML_BASE_LIB)
    target_link_libraries(WisdomLLMHost PRIVATE ${GGML_BASE_LIB})
  endif()
  # 在 Squirrel.app/Contents/MacOS 裡從 Contents/Frameworks 載入 llama.cpp 與 librime（安裝名稱是 @rpath 時）。
  # 建置資料夾裡的仍指向 LLAMA_ROOT；cmake --install 換成這個，鼠鬚管打包的是安裝後的版本
  set_target_properties(WisdomLLMHost PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
  install(TARGETS WisdomLLMHost RUNTIME DESTINATION .)

  # 本機模型的端到端測試（啟動同一資料夾的 WisdomLLMHost）：需要模型檔，不加進 ctest
  #   build/LocalModelSmoke <model.gguf> [n_gpu_layers]
  add_executable(LocalModelSmoke ${ROOT}/test/TestLLMHost/LocalModelSmoke.cpp)
  target_link_libraries(LocalModelSmoke PRIVATE wisdom_core)
  add_dependencies(LocalModelSmoke WisdomLLMHost)
endif()

# ---- 測試
enable_testing()
set(TEST_KEY "${CMAKE_BINARY_DIR}/test-keys/personal.key")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-keys")

# 文字規則、預測引擎、推薦、選字統計與加密紀錄（不需要 librime）
add_executable(TestIme
  ${ROOT}/test/TestIme/TestIme.cpp
  ${CORE}/ime/text_rules.cpp
  ${CORE}/ime/rescore.cpp
  ${CORE}/ime/prediction_engine.cpp
  ${CORE}/ime/choice_stats.cpp
  ${CORE}/ime/choice_log.cpp
  ${CORE}/crypto/personal_crypto.cpp
  ${CORE}/platform/mac/key_store_mac.cpp
  ${CORE}/third_party/monocypher/monocypher.c
)
target_include_directories(TestIme PRIVATE ${COMMON_INCLUDES})
target_compile_definitions(TestIme PRIVATE PERSONAL_CRYPTO_TEST_KEY_FILE="${TEST_KEY}")
target_link_libraries(TestIme PRIVATE ${SECURITY} ${COREFOUNDATION})
add_test(NAME TestIme COMMAND TestIme)

# 推理行程的管道與子行程（posix_spawn）：對測試用的 FakeHost
add_executable(FakeHost ${ROOT}/test/TestLLMHost/FakeHost.cpp ${CORE}/llm_ipc/host.cpp
                        ${CORE}/platform/posix/process_posix.cpp)
target_include_directories(FakeHost PRIVATE ${COMMON_INCLUDES})
add_executable(TestLLMHost ${ROOT}/test/TestLLMHost/TestLLMHost.cpp ${CORE}/llm_ipc/client.cpp
                           ${CORE}/platform/posix/process_posix.cpp)
target_include_directories(TestLLMHost PRIVATE ${COMMON_INCLUDES})
add_dependencies(TestLLMHost FakeHost)
add_test(NAME TestLLMHost COMMAND TestLLMHost)

if(RIME_LIB)
  # LLM 的 HTTP（libcurl）對 mock_server.py
  add_executable(TestLLMHttp ${ROOT}/test/TestLLMHttp/TestLLMHttp.cpp ${CORE}/llm/LLMProvider.cpp
                             ${CORE}/net/curl/http_curl.cpp)
  target_include_directories(TestLLMHttp PRIVATE ${COMMON_INCLUDES})
  target_link_libraries(TestLLMHttp PRIVATE CURL::libcurl ${RIME_LIB})
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME TestLLMHttp
             COMMAND ${ROOT}/test/TestLLMHttp/run_posix.sh ${Python3_EXECUTABLE} $<TARGET_FILE:TestLLMHttp>)
  endif()

  # 控制器：需要注音方案的資料（-DRIME_SHARED=<含 bopomofo.schema.yaml 的資料夾>）
  add_executable(TestController ${ROOT}/test/TestController/TestController.cpp)
  target_link_libraries(TestController PRIVATE wisdom_core)
  if(RIME_SHARED)
    add_test(NAME TestController COMMAND TestController ${RIME_SHARED} ${CMAKE_BINARY_DIR}/rime-user)
  endif()
endif()
