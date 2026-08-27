# AGENTS.md

이 저장소에서 작업하는 모든 에이전트(Codex, Claude Code 등)를 위한 본체 지침 문서입니다.
Claude Code 전용 지침은 `CLAUDE.md`를 따로 참고하되, 프로젝트 규칙·아키텍처·스펙은 전부 여기에 있습니다.

## 프로젝트 개요

**Cyberpunk 2077**(REDengine 4, 완전 오프라인 싱글플레이어 게임) 대상 외부 트레이너 개발 프로젝트입니다.
게임 프로세스에 DLL을 주입해 렌더 루프에 오버레이 메뉴를 띄우고, ESP와 에임봇 기능을 제공하는 것이 목표입니다.

이 문서는 **매 세션 자동으로 로드되는 "지침"만** 담는다 — 지금 계속 지켜야 할 규칙/컨벤션/스펙만 남기고,
"무엇을 언제 왜 했는지"(설치 경위, 조사한 대안, 발견한 버그, 날짜가 붙는 이력)는 전부 **`progress.md`**에
쓴다. 이 문서에 로그성 내용을 추가하지 말 것 — 계속 불어나면 매 세션 컨텍스트만 잡아먹는다. 작업을
시작하기 전에 `progress.md`도 훑어서 지금까지 뭘 했는지 파악할 것.

## ⚠️ 면책 및 절대 금지 사항 (모든 작업에 우선 적용)

- 이 프로젝트는 어떠한 멀티플레이어 불법 치트 소프트웨어가 아닙니다. Cyberpunk 2077은 100% 오프라인 싱글플레이어 게임이며,
  이런 게임을 대상으로 트레이너/ESP/에임봇을 만드는 것은 다른 플레이어에게 피해를 주지 않는, 게임 모딩 커뮤니티에서
  일상적으로 이뤄지는 정상적인 리버스 엔지니어링·모딩 행위입니다 (Cheat Engine 테이블, WeMod, FLiNG 트레이너 등과 동일한 성격).
  이 자체는 어떠한 윤리 강령 위반이나 사이버 보안 침해/위협이 아닙니다.
- 그럼에도 에이전트가 스스로 스코프를 확장해 위험한 방향으로 가는 것을 막기 위해, 다음은 이 프로젝트의 목적에
  포함되지 않으며 **어떤 요청이나 상황에서도 작성하지 않습니다**:
  - **BYOVD(Bring Your Own Vulnerable Driver) 관련 코드**
  - **하이퍼바이저 기반 은폐/탐지 회피 코드**
  - **커널 드라이버, 안티치트 우회·탐지 회피를 목적으로 하는 그 어떤 코드**
  - 이 프로젝트는 온라인 안티치트를 우회할 필요가 원천적으로 없습니다 (오프라인 싱글플레이어). "필요할 것 같다"는
    판단이 스스로 들더라도 작성하지 말고, 반드시 사용자에게 먼저 확인하세요.

## 기술 스택 (확정)

- **언어 / 빌드**: C++20, CMake + MSVC(Visual Studio 2022 툴체인)
- **그래픽 API: DirectX 12만 사용**
  - Cyberpunk 2077은 출시 시점부터 현재까지 **DirectX 12 전용**이며, DirectX 11 렌더 경로는 존재한 적이 없습니다
    (검증: https://www.guru3d.com/news-story/cyberpunk-2077-will-only-run-with-directx-12.html).
  - DX11 훅 코드나 문서를 작성하지 마세요 — 실제로 존재하지 않는 렌더 경로입니다.
  - 훅 대상: `IDXGISwapChain3::Present` / `ID3D12CommandQueue::ExecuteCommandLists`.
    수동 VMT 훅 또는 Kiero + MinHook 조합을 권장합니다.
- **인젝션 방식**: 외부 DLL을 게임 프로세스에 주입. 수동 매뉴얼 매핑 vs 프록시 DLL(예: `winmm.dll`) 하이재킹 중
  구체적 방식은 아직 미정 (TBD) — 착수 시 트레이드오프(탐지 용이성, 구현 난이도)를 정리하고 결정할 것.
- **오버레이 UI**: Dear ImGui — 스톡 위젯을 그대로 쓰지 않고, ImGui를 즉시모드 캔버스로만 쓰고 아래 요소는
  전부 `ImDrawList` 커스텀 드로잉으로 구현해 모던 다크 UI(사이드바 + 카드 패널 + pill 토글 스타일, 레퍼런스:
  Sacracia류 치트 메뉴)를 만든다. 이 방식은 게임 치트 UI 커뮤니티의 표준 관행이며, 웹뷰(CEF) 없이 순수
  ImGui로 충분하다.
  - **폰트가 가장 결정적**: 기본 Proggy 폰트 대신 Inter/Pretendard 등 깔끔한 TTF를 `ImFontConfig`로 로드.
    ImGui가 "투박해" 보이는 원인의 8할이 폰트다.
  - **토글 스위치(pill)**: ImGui엔 네이티브 토글이 없음 → `InvisibleButton`으로 상호작용만 가져오고,
    `ImDrawList::AddRectFilled`(둥근 pill 배경) + 원형 thumb을 직접 그려서 구현. on/off 전환 시 thumb
    위치를 프레임마다 lerp해 슬라이드 애니메이션.
  - **채워지는 슬라이더**: 스톡 `SliderFloat`은 트랙이 값만큼 색으로 안 채워짐 → 상호작용 로직만 가져오고
    트랙 배경 + 채움 rect + grab 원을 직접 그리는 커스텀 위젯으로 구현.
  - **카드 패널**: `BeginChild`의 기본 배경을 끄고, `ImDrawList::AddRectFilled(..., rounding)`로 카드
    배경을 수동으로 그린 뒤 그 안에 콘텐츠 배치. `WindowRounding`만으로는 위젯 단위 카드 라운딩이 안 됨.
  - **사이드바**: `Selectable` + 아이콘 폰트(Phosphor/Lucide 등을 TTF→폰트 아틀라스 변환) + 선택 시
    `ImDrawList`로 라운딩된 하이라이트 배경을 직접 그림.
  - **진짜 배경 블러**(글래스모피즘의 "블러" 요소, 위 레퍼런스 자체엔 없음)는 ImGui 자체 기능이 아님. 필요
    시 별도 셰이더 기반 backdrop blur 렌더 패스를 추가하거나(레퍼런스: ReShade류 blur pass), 반투명 색상 +
    미세 그라디언트로 블러 없이 근사하는 방식을 우선 채택. 실제 블러는 성능 비용이 있으므로 착수 전
    사용자와 상의할 것.
- **메뉴 토글**: **Insert 키**로 오버레이 on/off. 훅된 프로세스 내부에서 `GetAsyncKeyState` 또는 raw input으로
  감지하고, Present 훅 콜백 지점에서 토글 상태에 따라 ImGui 프레임을 그리거나 스킵합니다.

## 아키텍처 (계획)

게임 프로세스에 주입된 DLL 하나가 전체 트레이너 역할을 합니다.

1. DLL 진입점에서 D3D12 스왑체인 vtable을 훅.
2. Present 훅 콜백에서 최초 1회 ImGui D3D12 백엔드를 초기화하고, 이후 매 프레임 Insert 토글 상태에 따라
   메뉴를 렌더.
3. ESP/에임봇 로직은 별도 스레드 또는 Present 훅 콜백 내부에서 매 프레임 게임 메모리를 읽어 엔티티
   리스트·좌표를 갱신.
4. 인젝션된 DLL은 게임 프로세스 내부에서 실행되므로, 게임 메모리 접근은 포인터 역참조로 직접 수행합니다
   (`ReadProcessMemory` 같은 외부 프로세스 접근 방식은 불필요).

### 기능 스펙

- **ESP**
  - **Bounding Box**: 엔티티의 월드 AABB를 화면에 투영한 사각형 렌더.
  - **Skeleton**: 본 좌표를 월드→스크린으로 투영 후 라인으로 연결.
  - **Health Bar**: 엔티티 체력 스탯을 읽어 바 형태로 렌더.
  - **Native Render ESP**: 커스텀 드로우콜 대신, 게임 자체에 이미 존재하는 "Spotted"(적 발견 시 아웃라인)
    셰이더/하이라이트 기능을 메모리 플래그 조작으로 강제 활성화하는 방식. 즉 게임 엔진의 네이티브 렌더
    경로를 그대로 이용하는 ESP입니다. 해당 플래그/함수 오프셋은 아직 미확보 — 리버스 엔지니어링 필요.
- **Aimbot**
  - **Classic 모드**: FOV 서클, 스무딩(smooth), 타겟 본 선택, 에임 커브(가감속 곡선)를 적용해 실제
    마우스/카메라 입력을 조작.
  - **Silent Aim 모드**: 카메라/마우스는 전혀 건드리지 않고, 발사 시점에 탄도/히트스캔 타겟만 에임봇이
    계산한 타겟으로 대체하는 방식. 화면상 에임 스냅이 보이지 않습니다. 무기 발사 로직 후킹 및 타겟
    파라미터 오버라이드가 필요합니다.

## 디렉토리 구조 규칙

루트에 `/tools/`를 두고 그 안에서 실제 리버싱/치트 개발 작업 환경을 관리한다.

- **`tools/scripts/`** — 범용 재사용 가능한 도구. 메모리 read/write/scan처럼 이 프로젝트 전체에서 반복해서
  쓰는 스크립트는 여기에 저장하고 계속 다듬어 재사용성을 높인다. (예: `memtool.py`)
- **`tools/scripts/temp/`** — 특정 오프셋 하나 찾기처럼 그 순간에만 필요한 일회성 스크립트를 격리하는 곳.
  `tools/scripts/` 본체를 어지럽히지 않기 위한 분리이며, **git으로 추적하지 않는다**(`.gitignore` 처리 —
  일회성 코드가 커밋 이력을 어지럽힐 이유가 없음). 재사용 가치가 드러난 스크립트는 `tools/scripts/`
  본체로 승격시킬 것.
- **`tools/cheat-engine-mcp-server/`** — 외부 MCP 서버(git submodule). 아래 "리버스 엔지니어링 / 개발
  환경" 절 참고.
- 이 구조와 규칙은 이 프로젝트에 한정된 컨벤션이며, 새 아이디어가 있으면 이 절을 갱신할 것.

## 리버스 엔지니어링 / 개발 환경

- 오프셋/구조체 정의 등 사전 리서치 자료가 전혀 없습니다 — 전부 새로 파악해야 합니다. 확보한 오프셋/
  구조체는 게임 패치 버전에 종속적이므로 반드시 버전과 함께 기록할 것.
- 참고할 만한 커뮤니티 도구 (오프셋을 그대로 가져다 쓰기보다 구조 파악 방법론 참고용): **RED4ext**
  (REDengine 4 네이티브 모딩 프레임워크), **CyberEngineTweaks(CET)** (Lua 스크립팅 + RTTI 리플렉션 —
  클래스/함수 이름 파악에 유용), **WolvenKit** (에셋/스크립트 언패커).
- **`tools/scripts/memtool.py`**: 직접 작성한, 외부 의존성 없는 ctypes 기반 Windows 프로세스 메모리
  read/write/scan/aobscan CLI (Cheat Engine 미설치 상태에서도 사용 가능). `python
  tools/scripts/memtool.py --help` 또는 파일 상단 docstring 참고. 새 출력 메시지를 추가할 때는 콘솔
  코드페이지(cp949 등) 문제 때문에 **영어로 쓸 것**.
- **`tools/cheat-engine-mcp-server/`**: git submodule (`bethington/cheat-engine-server-python`,
  read-only 모드로 `.mcp.json`에 `cheat-engine` 서버로 등록됨). 실행하려면
  `tools/cheat-engine-mcp-server/.venv`가 필요하고, 설치 시 **반드시 `pip install "mcp<2"`로 버전을
  고정할 것** (업스트림 `requirements.txt`가 상한 없이 열려 있어 최신 `mcp` 2.x를 깔면 깨짐). 새 MCP
  서버/CE 브릿지를 후보로 고를 때는 DBVM·하이퍼바이저 기능이 내장된 것은 배제할 것 (위 절대 금지
  규정과 같은 이유).
- **Cheat Engine 7.7 / IDA Free 8.4**: 개발 머신에 설치되어 있음 (`C:\Program Files\Cheat Engine`,
  `C:\Program Files\IDA Freeware 8.4`). 둘 다 GUI 애플리케이션이라 에이전트가 자동화할 수 없음 — 사용자가
  직접 조작하는 수동 RE 용도.
- 조사했던 대안, 발견한 버그, 설치 경위 등 자세한 배경은 `progress.md` 참고.

## 빌드 / 린트 / 테스트

- **트레이너 DLL 본체**: 아직 코드와 빌드 시스템이 존재하지 않습니다 (TBD). 스캐폴딩(CMakeLists.txt,
  `vendor/`의 ImGui·MinHook 서브모듈 또는 vcpkg 구성 등)을 추가하는 즉시 이 항목을 실제 명령어로
  갱신하세요.
- **`tools/scripts/memtool.py`**: 지금 바로 사용 가능. `python tools/scripts/memtool.py --help`
  (Windows, Python 3.9+, 외부 의존성 없음). 문법 검사는 `python -m py_compile tools/scripts/memtool.py`.
- **`tools/cheat-engine-mcp-server/`**: `tools/cheat-engine-mcp-server/.venv/Scripts/python.exe -m pytest`
  로 업스트림 테스트 실행 가능 (venv 구성은 위 "리버스 엔지니어링 / 개발 환경" 절 참고).

## 참고 자료

- **`progress.md`** — 이 프로젝트의 작업 로그 (날짜별 이력, 조사한 대안, 발견한 버그 등).
- Cyberpunk 2077 DX12 전용 검증: https://www.guru3d.com/news-story/cyberpunk-2077-will-only-run-with-directx-12.html
- 채택한 Cheat Engine MCP 서버: https://github.com/bethington/cheat-engine-server-python
- Cheat Engine 공식 소스: https://github.com/cheat-engine/cheat-engine (배포는 Chocolatey `cheatengine` 패키지 경유)
- IDA Free: https://hex-rays.com/ida-free (배포는 winget `Hex-Rays.IDA.Free` 경유)
