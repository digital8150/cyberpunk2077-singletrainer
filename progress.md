# progress.md

작업 기록(개발 로그). `AGENTS.md`는 매 세션 자동으로 읽히는 "지침" 문서라 계속 불어나면 안 되고, 여기는
"무엇을 언제 왜 했는지"를 남기는 곳이다. 새 항목은 **파일 맨 아래에 날짜와 함께 추가**할 것. 결정의 배경/
비교했던 대안/발견한 버그처럼 다시 안 봐도 되는 디테일은 여기로, 지금도 계속 지켜야 하는 규칙만 AGENTS.md에.

## 2026-08-27

- 저장소 초기화: `AGENTS.md`/`CLAUDE.md` 작성 (프로젝트 스펙, 면책/금지 사항, DX12+ImGui 기술 스택,
  ESP/Aimbot 기능 스펙). `git init` 수행.
- ImGui로 모던 다크 UI(사이드바 + 카드 패널 + pill 토글, 레퍼런스: Sacracia류 치트 메뉴)가 가능한지
  질문받아 조사 — 스톡 위젯 대신 `ImDrawList` 커스텀 드로잉(토글/슬라이더/카드)이면 충분하다고 결론,
  구체적 구현 방식을 AGENTS.md 기술 스택 절에 기록.
- `/tools/` 디렉토리 컨벤션 확립: `tools/scripts/`(재사용 도구), `tools/scripts/temp/`(일회성, git 추적 안
  함).
- **`tools/scripts/memtool.py` 작성**: 외부 의존성 없는 ctypes 기반 Windows 프로세스 메모리 read/write/
  scan/aobscan CLI. 실제 백그라운드 프로세스(알려진 int32 값 보유)를 대상으로 `scan`이 후보 3개를
  정확히 찾아내는 것까지 검증. 콘솔 코드페이지(cp949)에서 한글이 깨지는 걸 발견해 모든 런타임 출력을
  영어로 고정.
- **Cheat Engine MCP 서버 조사/채택**: `drdon1234/cheat-engine-mcp`(1 commit, star 5, DBVM 하이퍼바이저
  툴 카테고리 내장), `sudohakan/cheatengine-mcp-bridge`(14 commits, star 8, 역시 DBVM 4종 내장),
  `bethington/cheat-engine-server-python`(10 commits, star 64, Cheat Engine 설치 불필요한 독립 서버,
  읽기 전용, 하이퍼바이저 기능 없음) 세 후보를 비교. 앞 둘은 이 프로젝트가 금지하는 하이퍼바이저 기능을
  기본 내장하고 있어 배제하고 **bethington 것을 `tools/cheat-engine-mcp-server/`에 git submodule로
  추가**.
  - 이 저장소 `manifest.json`의 author가 "Anthropic"으로 잘못 적혀 있는 걸 발견 — MCP 예제 매니페스트를
    복사하며 남은 보일러플레이트로 보임(`LICENSE`엔 실제 개인 이름 있음, 코드에 네트워크 유출/난독화
    등 의심 패턴 없음을 grep으로 확인). 악의적 정황은 아니라고 판단, 참고로만 남김.
  - venv(Python 3.11) 구성 후 `pip install -r requirements.txt` 했더니 `requirements.txt`가
    `mcp>=1.0.0`로 상한이 없어 `mcp` 2.x가 깔려 코드가 기대하는 1.x `FastMCP` API가 깨짐(→ `mcp<2`로
    재설치해 해결).
  - `server/main.py`가 `--debug` 시작 로그에서 존재하지 않는 CLI 옵션 `args.workspace`를 참조해 시작
    즉시 크래시하는 업스트림 버그 발견 → `getattr(args, 'workspace', ...)`로 방어하는 패치를 submodule
    안에 로컬 커밋(`379d5e8`, 원격엔 없음 — 다른 머신에서 `git submodule update --init`만 하면 이 패치는
    없으니 필요하면 재적용할 것)으로 남김. 이후 초기화~stdio 서버 attach 직전까지 정상 동작 확인
    (stdin이 없는 배경 프로세스로 테스트해서 stdio 단계에서만 실패 — 실제 MCP 클라이언트가 붙이면
    문제없을 것으로 예상, 세션 재시작 후 실사용 검증 필요).
  - `process_whitelist.json`에 `Cyberpunk2077.exe` 항목 추가 (게임 미설치 상태라 실사용 검증은 못 함).
  - 프로젝트 루트 `.mcp.json`에 `cheat-engine` 서버로 등록 (`--read-only`).
- **Cheat Engine 7.7 / IDA Free 8.4 설치**: 공식 배포처(cheatengine.org)는 가짜 "Download" 광고 버튼이
  섞여있는 것으로 알려져 있어 대신 Chocolatey 승인 패키지(체크섬 검증됨) 경로로, IDA Free는 winget
  공식 등재 패키지(해시 검증 통과) 경로로 설치 대상을 정함. 관리자 권한(UAC)이 필요해 에이전트가 직접
  완료하지 못했고, 사용자가 관리자 PowerShell에서 직접 설치 완료.
  - 설치 경로: `C:\Program Files\Cheat Engine\cheatengine-x86_64.exe`,
    `C:\Program Files\IDA Freeware 8.4\ida64.exe`.
- AGENTS.md에 개발 로그성 내용이 계속 쌓이는 문제를 사용자가 지적 — 이 파일(`progress.md`)로 분리하고
  AGENTS.md는 "매 세션 다시 봐도 되는 지침"만 남기도록 정리.
- **트레이너 DLL 스캐폴딩**: `vendor/imgui`, `vendor/minhook`을 git submodule로 추가(ImGui/MinHook 둘 다
  자체 CMake 지원이 imgui엔 없어서 CMakeLists.txt에서 core+win32/dx12 백엔드 소스를 직접 나열, minhook은
  자체 CMakeLists.txt의 `minhook` 타겟을 그대로 사용). `src/`에 DllMain → 별도 스레드에서 훅 초기화 →
  더미 디바이스/스왑체인으로 vtable 주소만 뽑아 `IDXGISwapChain::Present`(8)/`::ResizeBuffers`(13)/
  `ID3D12CommandQueue::ExecuteCommandLists`(10)를 MinHook으로 후킹 → Present 훅에서 ImGui DX12/Win32
  백엔드 지연 초기화 + Insert 키로 WndProc 후킹해 토글하는 구조로 작성.
  - 이 개발 머신에 **Visual Studio Community 2026(18.9.1) C++ 워크로드가 이미 설치되어 있었다** (MSVC
    14.51, Windows SDK 10). CMake는 없어서 `winget install --id Kitware.CMake --scope user`로 설치
    (관리자 권한 불필요, zip 압축 해제 방식이라 UAC 안 뜸 — CE/IDA와 달리 이건 자동 설치 성공함).
  - 실제로 `cmake -G "Visual Studio 18 2026" -A x64` + `cmake --build`로 끝까지 빌드해
    `build/bin/Release/cp2077_trainer.dll` 산출까지 검증함 (게임에 인젝션해서 실제로 뜨는지는 아직
    미검증 — 게임이 이 머신에 없음).
  - 빌드 중 발견/수정한 문제:
    - `vendor/imgui`가 master 최신본이라 `ImGui_ImplDX12_Init`이 예전의 "포인터 여러 개 넘기는" 시그니처가
      아니라 `ImGui_ImplDX12_InitInfo` 구조체 + SRV 디스크립터 alloc/free 콜백을 요구하는 새 API로 바뀌어
      있었음(1.92+, 동적 텍스처 지원 때문에). `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`를 정의해 놔서 레거시
      오버로드도 안 보였음 — 정공법으로 새 API에 맞춰 SRV 힙을 64개로 늘리고 간단한 슬롯 할당기를 구현.
    - `WIN32_LEAN_AND_MEAN`/`NOMINMAX`를 CMake 컴파일 정의와 `framework.h` 양쪽에서 정의해 C4005
      재정의 경고 → CMake 쪽 제거, 헤더 쪽만 유지.
    - 소스가 UTF-8(한글 주석)인데 MSVC가 기본 cp949로 해석해 C4819 경고 → `/utf-8` 컴파일 옵션 추가.
  - `ResizeBuffers` 처리, 멀티 커맨드큐 환경에서 "렌더용 큐" 특정 등은 TODO로 남겨둠 — 실제 게임에서
    검증한 뒤 다듬을 것.
- **실제 게임에 첫 인젝션 테스트**: 사용자가 Cyberpunk2077.exe를 직접 실행해 둠 (pid 30220, `d3d12.dll`/
  `D3D12Core.dll`만 로드되고 `d3d11.dll`은 없음을 확인 — DX12 전용이라는 AGENTS.md 기술 스택 절 내용의
  실물 검증). `tools/scripts/inject.py`(`LoadLibraryW`+`CreateRemoteThread` 방식 인젝터, 새로 작성)로
  `cp2077_trainer.dll`을 주입 — 게임 크래시 없이 모듈 목록에 정상적으로 나타남.
  - 첫 시도는 `WriteProcessMemory failed (err=998)`로 실패. 원인은 ctypes가 `VirtualAllocEx`의 반환값을
    `.restype` 미지정 시 기본 32비트 `c_int`로 취급해 실제 64비트 주소의 상위 비트를 잘라먹은 것 —
    `restype`/`argtypes`를 명시해 해결(같은 문제가 잠재해 있던 `memtool.py`의 `OpenProcess`에도 방어적
    으로 적용).
  - 오버레이가 실제로 화면에 그려지는지(Insert 토글 포함)는 사용자 육안 확인 대기 중.
- **초기 라이브 테스트 3회의 크래시/행 조사**:
  - 1차는 오버레이와 Insert 토글까지 동작한 뒤 GPU watchdog/TDR 정황과 함께 게임이 종료됨. 오버레이의
    커맨드 얼로케이터를 GPU 완료 펜스 없이 Reset하던 D3D12 스펙 위반을 발견해 프레임별 펜스 동기화를
    추가하고, 마우스 커서 표시/입력 차단/한글 폰트 로드도 함께 수정(`9f4ed11`).
  - 2차는 인젝션 직후 하드 프리즈. `ExecuteCommandLists`에서 처음 관측한 큐의 타입을 확인하지 않아
    Compute/Copy 큐에 Direct 커맨드리스트를 제출할 수 있던 문제를 발견해 Direct 큐 필터와 펜스 대기
    타임아웃을 추가했으나, 3차에도 프로세스가 수 초 안에 종료되어 이것만으로는 원인을 해결하지 못함.
  - 이 시점까지 실제 콜스택/내부 로그 없이 이벤트 로그와 증상만으로 추정해 왔으므로, 다음 수정부터는
    DLL 자체 진단 로그와 HRESULT/device-removed 정보를 먼저 확보하기로 함.
- **진단 계측 및 네 번째 라이브 인젝션 성공**:
  - `src/diagnostics.*`를 추가해 DLL 옆 `cp2077_trainer.log`와 `OutputDebugStringA`에 훅 설치, Direct 큐
    관측, Present, 스왑체인 정보, ImGui/D3D12 초기화 단계, 첫 프레임 제출, HRESULT 실패, 펜스 타임아웃,
    `GetDeviceRemovedReason`/DRED 정보를 즉시 flush하도록 함. 초기화가 중간에 실패해도 만들어진 ImGui/
    D3D12 리소스를 정리하고 렌더를 fail-closed하도록 실패 경로도 보강.
  - 프로세스 내 여러 스왑체인 Present가 한 훅으로 들어오는 위험을 막기 위해 최초 성공한 스왑체인
    인스턴스에 오버레이를 고정하고, 다른 스왑체인의 Present/ResizeBuffers는 무시하도록 변경.
  - "처음 관측된 Direct 큐" 대신 **Present와 같은 스레드에서 마지막으로 관측된 Direct 큐**를 사용하도록
    변경. 실제 PID 31492 로그에서 첫 Direct 큐는 `0x...96AAAAF0`, Present 스레드의 큐는
    `0x...8D16E210`으로 서로 달랐음. 즉 이전 Direct 타입 필터 뒤에도 잘못된 큐를 선택하고 있었음이
    실측으로 확인되었고, 이 큐 불일치가 앞선 GPU 행/프로세스 종료의 가장 강한 원인 후보임.
  - 스왑체인의 실제 포맷을 ImGui PSO/RTV에 사용하도록 수정. 이번 게임 설정의 실측 포맷은 28
    (`DXGI_FORMAT_R8G8B8A8_UNORM`), 해상도 2560x1440, 백버퍼 2개였으므로 기존 하드코딩이 이번 크래시의
    직접 원인은 아니었지만 HDR 등 다른 설정에서의 device removal 가능성을 제거함.
  - 새 Release DLL 주입 후 훅/오버레이 초기화와 첫 프레임 제출 성공, 10초 이상 게임 응답 유지, Insert로
    `visible=0` 토글까지 로그로 확인. 실패/펜스 타임아웃/device removal 로그 없음. 빌드는 MSVC `/W4`
    override에서도 경고 없이 통과.
  - 테스트 도중 `inject.py --help`가 cp949 콘솔에서 한글/긴 대시 때문에 `UnicodeEncodeError`를 내는 기존
    문제를 발견해 argparse 설명과 help 문자열을 ASCII 영문으로 고정. `py_compile`과 `--help` 재검증 통과.
- **주요 작업 기점 자동 커밋 규칙 도입**: 기능 단위 완료/크래시 수정/리버싱 결과 확정 시 관련 검사와
  `progress.md` 기록 후 자동 커밋하되, 검증되지 않은 중간 상태나 사용자 변경은 섞지 않고 push는 명시적
  요청 때만 하도록 `AGENTS.md`에 상시 규칙을 추가. 네 번째 인젝션까지 검증된 안정 오버레이 기준점을
  `9c1f024`(`Stabilize DX12 overlay and add crash diagnostics`)로 커밋함.
- **Cyberpunk 2077 2.31 엔티티 경로 조사 및 첫 기능 기반 구현**:
  - 실행 파일 제품 버전은 2.31, 파일 버전 `3.0.5294808`, CET가 기록한 내부 버전은 `3.0.80.51928`.
    게임 설치 폴더의 `cyberpunk2077_addresses.json`과 공식 RED4ext SDK/CET 소스를 대조해
    `world::RuntimeEntityRegistry::RegisterEntity` 주소 해시 `2840271332`, 호출 시그니처
    `void(IScriptable*, ent::Entity*)`를 확인함.
  - 현재 게임 프로세스에서 RegisterEntity 후보 주소의 함수 프롤로그를 직접 읽고, 상대 call 부분만
    wildcard 처리한 AOB가 `.text`에서 정확히 1회 매치함을 `memtool.py aobscan`으로 확인. RED4ext가 있으면
    공식 `RED4ext_ResolveAddress` export를 우선 쓰고, 없으면 이 유일 AOB를 쓰는 내부 시그니처 스캐너와
    선택적 MinHook 엔티티 등록 훅을 구현함.
  - 공식 SDK 레이아웃을 기준으로 `ent::Entity`의 native type(+0x30), entity ID(+0x48), transform component
    (+0xB0), `IPlacedComponent::worldTransform`(+0xE0), 고정소수점 WorldPosition 변환을 구현. 수명이 불명확한
    엔티티 포인터는 보관하지 않고 등록/위치 보유/puppet 분류 카운터와 마지막 위치 스냅샷만 원자적으로
    게시해 오버레이에서 진단 가능하게 함.
  - 기능 설정을 `Features::Settings`로 분리하고 ESP 세부 토글, 에임봇/FOV 원/반경/스무딩 UI 및 커스텀
    filled slider를 추가. 메뉴가 숨겨져도 ImGui 프레임과 기능 오버레이는 계속 렌더하도록 Present 경로를
    분리했으며, 현재 실제 화면 기능은 조절 가능한 FOV 원까지 구현됨. 엔티티 월드→스크린 투영, 생존 목록,
    box/bone/health 렌더는 다음 단계.
  - 게임 재시작 후 새 DLL을 PID 21768에 단독 주입. RED4ext가 RegisterEntity를
    `0x00007FF7524E57F4`로 해석했고 여러 게임 스레드에서 실제 등록 콜백이 발생해 첫 세션 256개까지
    카운트됨. 위치 변환 결과도 현재 월드 좌표 범위와 일관됐고 `gameObject` 등 실제 클래스 해시를 콜백
    시점에 확인함. 첫 오버레이 프레임, Insert 토글, 프로세스 응답 유지 및 실패/device removal 로그 없음.
    현재 세션 중간에 훅을 설치하므로 이미 등록된 NPC는 잡히지 않을 수 있어, 기존 registry 열거 또는 더
    이른 생명주기 훅이 다음 ESP 단계에 필요함.
- **안전 언로드/반복 주입 경로 구현**:
  - `End` 키 또는 프로세스별 named event가 전용 작업 스레드에 종료를 요청한다. 종료 시 먼저 MinHook을
    비활성화하고 Present/Resize/ExecuteCommandLists/엔티티/WndProc 콜백의 실행 중 카운터가 0이 되는지
    확인한 다음 WndProc, ImGui, D3D12 자원을 정리하고 `FreeLibraryAndExitThread`를 호출함. 로더 락 아래인
    `DllMain(DLL_PROCESS_DETACH)`에서는 무거운 정리를 하지 않도록 변경.
  - `inject.py --unload`가 named event만 신호하고 모듈이 실제로 사라질 때까지 확인하도록 추가. 안전 종료를
    지원하지 않는 구 DLL에는 `FreeLibrary`를 강제로 호출하지 않고 재시작 필요 오류를 반환함. 현재 게임의
    구 DLL에 이 실패 경로를 실행해 게임이 계속 응답함을 확인함.
  - 새 DLL을 별도 Python 호스트에 두 차례 실제 로드해 D3D12 훅 설치와 optional 엔티티 훅 fail-closed를
    거친 뒤 `inject.py --unload`를 호출함. 두 차례 모두 훅 종료 로그가 완결되고 DLL이 모듈 목록에서
    사라졌으며 호스트 프로세스가 정상 응답함을 확인.
  - PID 21768 실제 게임에서도 `inject.py --unload` 실행 후 훅 종료/자원 정리/diagnostics shutdown 로그가
    모두 완결되고 DLL이 모듈 목록에서 사라졌으며 게임이 계속 응답함을 확인. 같은 게임 프로세스를 종료하지
    않고 계측 추가→Release 재빌드→재주입까지 성공해 앞으로의 반복 개발 경로를 실물 검증함.
- **REDengine 카메라 월드→스크린 투영 연결 및 NPC 진단 마커**:
  - 오픈소스 RedHotTools의 현재 구현을 조사해 `gameICameraSystem` 내부 카메라(+0x60)와 주소 라이브러리
    `Camera::ProjectPoint` 해시 `1517361120`을 사용한다는 것을 확인. RED4ext의 `CGameEngine`/RTTI 주소
    해석으로 카메라 시스템을 얻는 최소 브리지를 `src/game/projection.*`에 구현하고 clip 좌표를 ImGui 픽셀
    좌표로 변환함.
  - PID 21768에서 카메라 시스템과 `ProjectPoint=0x00007FF752118600` 해석에 성공. 실제 월드 위치가 앞/뒤
    판정과 clip/screen 좌표로 변환되는 것을 로그로 확인했으며 프로세스는 정상 응답함.
  - 엔티티 등록 중 `NPCPuppet` CName 해시 `0x317316F0865EA816`이 실제로 잡혀 puppet 분류가 동작함을 확인.
    수명이 불명확한 포인터 대신 마지막 등록 NPC의 ID/위치 스냅샷을 별도로 저장하고, ESP가 켜지면 그 위치를
    초록색 진단 마커로 투영하도록 추가. 지속 추적 목록/이동 갱신은 아직 미구현.
- **정상 GPU 지연을 오버레이 치명 오류로 오인하던 문제 수정**:
  - 라이브 재주입 뒤 overlay fence가 `wanted=11, completed=10`인 상태로 100ms를 넘자 device status는
    정상인데도 렌더를 영구 비활성화하는 현상을 실측. CPU/GPU 프레임이 잠시 벌어지는 정상 상태를 오류로
    취급한 것이 원인.
  - 미완료 allocator는 절대 Reset하지 않되 Present 스레드를 기다리게 하지 않고 해당 오버레이 프레임만
    즉시 건너뛰도록 변경. 수정본에서 `wanted=3, completed=2` 지연이 다시 발생했지만 다음 프레임들 및 Insert
    토글이 계속 정상 동작했고 영구 비활성화/device error가 없음을 확인함.
