# AGENTS.md

이 저장소에서 작업하는 모든 에이전트(Codex, Claude Code 등)를 위한 본체 지침 문서입니다.
Claude Code 전용 지침은 `CLAUDE.md`를 따로 참고하되, 프로젝트 규칙·아키텍처·스펙은 전부 여기에 있습니다.

## 프로젝트 개요

**Cyberpunk 2077**(REDengine 4, 완전 오프라인 싱글플레이어 게임) 대상 외부 트레이너 개발 프로젝트입니다.
게임 프로세스에 DLL을 주입해 렌더 루프에 오버레이 메뉴를 띄우고, ESP와 에임봇 기능을 제공하는 것이 목표입니다.

현재 저장소는 비어 있는 초기 상태입니다 (코드/빌드 시스템 미존재). 이 문서는 앞으로의 작업을 위한 스펙 겸 가이드입니다.

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
- **`tools/cheat-engine-mcp-server/`** — 외부 MCP 서버(git submodule). 아래 "리버스 엔지니어링" 절 참고.
- 이 구조와 규칙은 이 프로젝트에 한정된 컨벤션이며, 새 아이디어가 있으면 이 절을 갱신할 것.

## 리버스 엔지니어링 / 개발 환경

- 오프셋/구조체 정의 등 사전 리서치 자료가 전혀 없습니다 — 전부 새로 파악해야 합니다.
- 참고할 만한 커뮤니티 도구 (오프셋을 그대로 가져다 쓰기보다 구조 파악 방법론 참고용):
  - **RED4ext** — REDengine 4 네이티브 모딩 프레임워크.
  - **CyberEngineTweaks(CET)** — Lua 스크립팅 + RTTI 리플렉션 노출. 클래스/함수 이름 파악에 유용.
  - **WolvenKit** — 에셋/스크립트 언패커.
- 확보한 오프셋/구조체는 게임 패치 버전에 종속적이므로, 반드시 버전과 함께 기록할 것 (예: 버전별 문서
  분리 등 — 구체적 관리 방식은 착수 시 결정).

### `tools/scripts/memtool.py` — 경량 메모리 read/write/scan CLI

Cheat Engine 설치 여부와 무관하게 Windows `ctypes`만으로 동작하는, 직접 작성한 범용 프로세스 메모리 도구.
외부 의존성 없음(표준 라이브러리만 사용), 관리자 권한으로 실행 시 대부분의 프로세스에 접근 가능.

- 서브커맨드: `list`(프로세스 목록), `modules`(모듈/베이스주소), `scan`(첫 스캔), `rescan`(변경/증가/감소/
  정확값으로 후보 좁히기 — 세션은 `tools/scripts/.memtool_state/<pid>.json`에 저장, git 추적 안 함),
  `read`, `write`, `aobscan`(`??` 와일드카드 지원 바이트 패턴 스캔, `--module`로 범위 제한 가능).
- 실행 출력은 콘솔 코드페이지(cp949 등)에서 한글이 깨지는 걸 피하려고 전부 영어로 고정되어 있다 — 새
  메시지를 추가할 때도 이 규칙을 따를 것.
- 실제 프로세스를 대상으로 scan/rescan 정상 동작 검증 완료 (2026-08-27).
- 사용 예시: `python tools/scripts/memtool.py --help` 또는 파일 상단 docstring 참고.

### Cheat Engine MCP 브릿지 (`tools/cheat-engine-mcp-server/`)

AI 에이전트를 Cheat Engine류 기능에 연결하는 MCP 서버 후보를 조사한 결과, 여러 프로젝트가 있었다
(`drdon1234/cheat-engine-mcp`, `sudohakan/cheatengine-mcp-bridge`, `bethington/cheat-engine-server-python`
등). **`bethington/cheat-engine-server-python`을 채택**해 git submodule로 추가했다. 이유:

- 다른 두 후보는 실제 Cheat Engine 설치 + Lua 브릿지가 필요하고, **DBVM(하이퍼바이저) 툴 카테고리를
  기본 내장**하고 있어(Ring -1 tracing, code cloaking 등) 이 프로젝트가 절대 금지하는 하이퍼바이저 관련
  기능과 같은 배에 타게 된다. 실제로 그 툴을 호출하지 않더라도, 애초에 그런 기능이 설치된 상태 자체를
  피하는 게 이 프로젝트의 금지 규정 취지에 맞다.
- 채택한 쪽은 **Cheat Engine 설치가 불필요한 독립 실행형 서버**이고, **읽기 전용**(`--read-only`)으로
  운용하며, 하이퍼바이저/DBVM 기능이 없다. GitHub star/커밋 이력도 가장 활발했다 (다른 후보들은 1~14
  커밋, star 5~8개 수준의 매우 신생/소규모 프로젝트였음).
- ⚠️ 참고용 메모: 이 저장소의 `manifest.json`에는 author가 "Anthropic"으로 적혀 있는데, 이건 실제
  Anthropic 프로젝트가 아니라 MCP 예제 매니페스트를 복사하며 남은 것으로 보이는 보일러플레이트다
  (`LICENSE`엔 실제 개인 이름이 있고, 코드에서 네트워크 유출/난독화 등 의심 패턴은 없음을 확인함). 그냥
  기록으로 남겨둔다.

**로컬 패치 (submodule 안에 로컬 커밋으로 존재, 원격엔 없음)**:
- `server/main.py`의 `--debug` 시작 로그가 존재하지 않는 `args.workspace`를 참조해 시작 즉시 크래시하는
  업스트림 버그를 고쳤다 (`getattr(args, 'workspace', ...)`로 방어). submodule 커밋 `379d5e8`.
- `requirements.txt`가 `mcp>=1.0.0`로 상한 없이 열려 있어 최신 `mcp` 2.x가 설치되면 코드가 기대하는
  `FastMCP` API(1.x)가 깨진다. **반드시 `mcp<2`로 설치할 것** (아래 설정 절차에 반영됨).
- `process_whitelist.json`에 `Cyberpunk2077.exe` 항목을 추가해 두었다 (게임이 아직 이 머신에 설치되어
  있지 않아 실사용 검증은 못 함).
- 다른 머신에서 `git submodule update --init`만 하면 이 로컬 커밋은 없다 — 필요하면 위 내용을 그대로
  재적용할 것.

**설정 절차** (이미 완료됨, 재현/참고용):
```powershell
cd tools/cheat-engine-mcp-server
python -m venv .venv        # Python 3.11 사용 (3.14는 capstone 등 휠 호환성 미검증)
.venv\Scripts\pip install -r requirements.txt
.venv\Scripts\pip install "mcp<2"   # 위 버전 고정 이유 참고
```
프로젝트 루트 `.mcp.json`에 `cheat-engine` 서버로 등록되어 있다 (`--read-only` 고정). stdio 트랜스포트
초기화까지 정상 동작 확인함 (whitelist 로드, launcher 초기화 등 로그 확인) — 실제 MCP 클라이언트가
stdin/stdout을 붙여줘야 완전히 붙는다.

### Cheat Engine (GUI) / IDA Free — 설치 완료 (2026-08-27)

- **Cheat Engine 7.7**: `C:\Program Files\Cheat Engine\cheatengine-x86_64.exe`. 공식 배포처
  (cheatengine.org)는 가짜 "Download" 광고 버튼이 섞여있는 것으로 알려져 있어, 대신 Chocolatey의
  승인(moderated)된 `cheatengine` 패키지(체크섬 검증됨) 경로로 설치했다.
- **IDA Free 8.4**: `C:\Program Files\IDA Freeware 8.4\ida64.exe`. Hex-Rays 공식 무료 버전, winget에
  정식 등재(`Hex-Rays.IDA.Free`)된 걸 설치 — 해시 검증까지 winget이 통과시킨 공식 CDN(out7.hex-rays.com)
  파일.
- 둘 다 관리자 권한(UAC)이 필요해 에이전트가 직접 설치를 완료하지 못하고, 사용자가 관리자 PowerShell에서
  직접 실행해 설치를 마쳤다.
- 이 두 도구는 **GUI 애플리케이션**이라 에이전트가 자동화할 수 없다 — 사용자가 직접 열어서 조작하는
  용도(수동 RE, `tools/cheat-engine-mcp-server`의 자동화 툴로 커버되지 않는 탐색 작업)로 존재한다.

## 빌드 / 린트 / 테스트

- **트레이너 DLL 본체**: 아직 코드와 빌드 시스템이 존재하지 않습니다 (TBD). 스캐폴딩(CMakeLists.txt,
  `vendor/`의 ImGui·MinHook 서브모듈 또는 vcpkg 구성 등)을 추가하는 즉시 이 항목을 실제 명령어로
  갱신하세요.
- **`tools/scripts/memtool.py`**: 지금 바로 사용 가능. `python tools/scripts/memtool.py --help`
  (Windows, Python 3.9+, 외부 의존성 없음). 문법 검사는 `python -m py_compile tools/scripts/memtool.py`.
- **`tools/cheat-engine-mcp-server/`**: `tools/cheat-engine-mcp-server/.venv/Scripts/python.exe -m pytest`
  로 업스트림 테스트 실행 가능 (구성/실행 방법은 위 "Cheat Engine MCP 브릿지" 절 참고).

## 참고 자료

- Cyberpunk 2077 DX12 전용 검증: https://www.guru3d.com/news-story/cyberpunk-2077-will-only-run-with-directx-12.html
- 채택한 Cheat Engine MCP 서버: https://github.com/bethington/cheat-engine-server-python
- Cheat Engine 공식 소스: https://github.com/cheat-engine/cheat-engine (배포는 Chocolatey `cheatengine` 패키지 경유)
- IDA Free: https://hex-rays.com/ida-free (배포는 winget `Hex-Rays.IDA.Free` 경유)
