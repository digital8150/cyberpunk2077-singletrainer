cmake_minimum_required(VERSION 3.21)
project(cp2077_singletrainer LANGUAGES CXX C)

if(NOT WIN32)
    message(FATAL_ERROR "이 프로젝트는 Windows + DirectX 12 전용입니다 (AGENTS.md 참고).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- MinHook (vendor/minhook, 자체 CMakeLists.txt 보유) ---
add_subdirectory(vendor/minhook)

# --- Dear ImGui: 코어 + Win32/DX12 백엔드만 정적 라이브러리로 빌드 (vendor/imgui엔 CMake 파일이 없음) ---
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# --- 트레이너 DLL 본체 ---
add_library(cp2077_trainer SHARED
    src/dllmain.cpp
    src/config.cpp
    src/diagnostics.cpp
    src/features/features.cpp
    src/features/aimbot.cpp
    src/features/esp.cpp
    src/features/fps_counter.cpp
    src/game/aim_assist.cpp
    src/game/animation_data.cpp
    src/game/rtti_invoker.cpp
    src/game/signature_scanner.cpp
    src/game/entity_tracker.cpp
    src/game/player_modifiers.cpp
    src/game/projection.cpp
    src/game/visibility.cpp
    src/hooks/cursor_hook.cpp
    src/hooks/hook_lifecycle.cpp
    src/hooks/d3d12_hook.cpp
    src/ui/overlay.cpp
    src/ui/widgets.cpp
)

target_include_directories(cp2077_trainer PRIVATE src)
target_compile_definitions(cp2077_trainer PRIVATE
    UNICODE
    _UNICODE
)
if(MSVC)
    # 소스 파일이 UTF-8(한글 주석 포함)이라 MSVC에 소스/실행 문자셋을 명시한다 (C4819 경고 방지).
    target_compile_options(cp2077_trainer PRIVATE /utf-8)
endif()
target_link_libraries(cp2077_trainer PRIVATE
    imgui
    minhook
    d3d12.lib
    dxgi.lib
    user32.lib
)

set_target_properties(cp2077_trainer PROPERTIES OUTPUT_NAME "cp2077_trainer")
