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
- **메뉴 커서 고정 해제, FPS 카운터, 지속 NPC 박스 프로토타입 구현**:
  - 게임이 플레이 중 `ClipCursor`와 `SetCursorPos` 계열 API로 커서를 중앙에 다시 고정하는 동작을 메뉴가
    열린 동안만 억제하는 MinHook 기반 cursor hook을 추가. 메뉴를 열 때 기존 clip을 즉시 풀고, 닫거나
    언로드할 때 억제를 해제해 게임이 다음 프레임부터 원래 커서 제어를 복구할 수 있게 함. 현재 Windows의
    `SetCursorPos`/`SetPhysicalCursorPos` export가 같은 주소를 공유해 두 번째 훅 생성이
    `MH_ERROR_ALREADY_CREATED`가 되는 것도 실주입 로그로 발견해 별칭으로 명시 처리함.
  - ImGui 측 평균 프레임률을 사용하는 우상단 FPS 배지를 추가하고 메뉴에서 표시 여부를 바꿀 수 있게 함.
  - 등록된 NPC를 최대 256개까지 ID와 포인터로 추적하고, 매 프레임 사용할 때 ID/클래스/위치를 다시
    검증한 값 스냅샷만 렌더러에 전달하도록 구현. 스트리밍으로 해제된 포인터 접근은 SEH 경계에서 격리하고
    목록에서 제거하며 PlayerPuppet은 제외함. 최대 128개 스냅샷을 월드→스크린 투영해 현재는 1.8m 가상
    높이를 쓰는 근사 박스와 ID 라벨을 그린다. 실제 AABB/본/체력 접근은 아직 리버싱되지 않아 UI에도
    프로토타입임을 명시함.
  - `build-next`와 정식 `build/bin/Release` 모두 전체 Release 빌드 통과. PID 21768에 실주입해 cursor hooks
    `complete=1`, 메뉴 capture on/off, 첫 오버레이 프레임, 정상 GPU 지연 시 프레임 skip, NPC 등록 및
    월드 투영이 모두 로그에 나타나고 게임이 계속 응답함을 확인. 실제 커서 조작성/FPS 배지/박스의 시각적
    최종 확인은 사용자 인게임 확인을 기다리는 중.
- **고FPS/DLSS Frame Generation 환경의 오버레이 깜빡임 해결**:
  - 사용자가 메뉴 커서, FPS 배지, NPC 박스 세 항목을 모두 인게임 통과로 확인한 뒤, 게임 포커스가 있고
    약 150 FPS일 때 메뉴와 ESP가 함께 깜빡이며 포커스를 잃어 약 89 FPS가 되면 사라지는 현상을 보고함.
    기존 구현은 swap-chain back buffer마다 command allocator 하나만 두고 해당 allocator의 fence가 아직
    완료되지 않았으면 그 오버레이 프레임을 건너뛰었으므로, 높은 프레임률과 비동기 Present에서 실제 게임
    프레임은 계속 나오지만 오버레이 제출만 교대로 누락되는 구조였음.
  - back buffer와 allocator 수명을 분리하고 초기 4개, 필요 시 최대 32개까지 늘어나는 allocator pool에서
    완료된 항목만 Reset하도록 변경. 처음 검증된 렌더 command queue를 보유해 비동기 Present에서도 같은
    queue를 사용하고, Present/Resize를 직렬화했으며 5초 단위 cadence 계측을 추가함.
  - PID 21768의 약 150 FPS 라이브 로그에서 `presents=742 submitted=742 allocatorMisses=0 pool=4` 등으로
    모든 Present에 오버레이가 제출됐고, 사용자가 깜빡임이 해결됐다고 확인함.
- **NPC 시민/적/경찰 분류 및 거리 필터 진단 구현**:
  - 게임 2.31 REDmod 스크립트의 `ScriptedPuppet`에서 `IsCharacterCivilian`, `IsCharacterPolice`,
    `IsCharacterGanger`가 각각 캐시된 private bool을 반환함을 확인. 런타임 CClass의 일반 property 조회에서
    처음에는 `m_is*` 이름이 나오지 않았으나, 세 getter의 linked bytecode가 모두
    `Return(0x27), ObjectField(0x1A), CProperty*` 형식임을 실메모리에서 확인함. 런타임 property 이름은
    접두사 없는 `isCivilian/isPolice/isGanger`이며 value-holder 오프셋은 각각
    `0x3E4/0x3E5/0x3E6`이었음.
  - 고정 오프셋 대신 매 실행 RTTI 조회를 우선하고, 실패 시 getter bytecode의 CProperty를 해석하는 fallback을
    구현. 시민은 초록색, 갱 계열 적은 빨간색, 경찰은 파란색, 아직 분류되지 않은 puppet은 회색으로 표시하고
    각각 독립 토글 및 10~300m 거리 슬라이더를 추가함.
  - 월드 투영의 양수 clip W(카메라 전방 깊이)를 현재 거리값으로 노출하고, 3초 단위 로그에 카테고리 수,
    투영/전방/거리 초과/거리 통과/실제 draw 수와 전방 깊이 범위를 기록하도록 구현. PID 21768 라이브 값은
    `categories[civilian=0 enemy=0 police=1 other=1]`, `depthRange=[33.74,78.93]`,
    `maxDistance=100.0`, `distanceRejected=0`, `withinDistance=2`였으므로 당시 미표시 원인은 거리 필터가
    아니라 잘못된 분류 이름이었음을 확인함. 재주입으로 설정이 기본값(ESP off, 100m)으로 초기화되는 점은
    사용자에게 안내했으며, 최신 분류 색상의 시각적 확인은 다음 인게임 피드백에서 이어갈 것.
  - `build-next` 및 정식 `build/bin/Release` 전체 Release 빌드를 통과했고, 최신 검증 DLL을 PID 21768에
    안전 언로드/재주입한 상태에서 게임 응답, 약 125 FPS cadence, allocator miss 0을 확인함.
- **설정 자동 저장/로드와 숨김 시작 UX 구현**:
  - Windows INI API로 `%LOCALAPPDATA%\\cbpk\\config.ini`를 만들고 FPS, ESP 표시 항목/카테고리/거리,
    네이티브 하이라이트, 사망 대상 숨김, 에임봇 대상/FOV/스무딩/거리를 500ms debounce로 자동 저장함.
    DLL 안전 종료 때 미저장 변경을 flush하며, PID 21768에서 파일 생성과 여러 UI 변경의 즉시 저장,
    안전 재주입 뒤 `config loaded` 및 값 복원을 확인함.
  - DLL 로드 시 메뉴 기본 상태를 닫힘으로 바꾸고, 화면 상단에 커스텀 draw-list 패널이 cubic ease-in-out으로
    0.55초 slide-in, 3초 유지, 0.55초 slide-out하며 Insert 안내를 표시하도록 구현. 재주입 뒤 저장된 기능은
    복원되지만 메뉴 capture/toggle 로그 없이 닫힌 상태로 첫 프레임이 제출됨을 확인함.
- **애니메이션 시스템 기반 실제 AABB/rig 스켈레톤 ESP 구현**:
  - 공식 RED4ext SDK/Codeware 런타임 시스템 매핑을 기준으로 `worldAnimationSystem` 인덱스 7과
    `AnimatedEntitiesBucket`의 EntityID→index 맵을 직접 읽는 `src/game/animation_data.*`를 추가. 처음엔
    상속 베이스 뒤 bucket 시작을 0x80으로 오인했으나 실메모리에서 `system+0x50`에
    `size=54/capacity=211/stride=0x18` 맵이 있음을 확인해 수정함. 공개 SDK에 없는 EntityID 전용 hasher는
    추정에 의존하지 않고 활성 hash-chain 전체를 순회하는 fallback으로 처리함.
  - animation bounds의 8개 꼭짓점을 전부 게임 카메라로 투영해 실제 화면 사각형을 만들고, 데이터가 없는
    대상만 기존 1.8m 근사 박스로 fallback함. MetaRig의 bone transform/parent index를 엔티티 월드 회전과
    합성하고 독립 AABB 안에 들어오는지 검증한 뒤 최대 64개 parent-child 선분을 스켈레톤으로 그림.
  - PID 21768에서 `animation bounds resolved`가 실제 NPC ID/인덱스와 약 1.8m 높이의 월드 min/max를
    기록했고 `animation skeleton resolved: segments=64`, ESP 진단 `realBounds=8/8`을 확인. 게임은 계속
    응답하고 DX12 allocator miss/device 오류가 없음.
- **네이티브 하이라이트, 사망 필터, 클래식 에임봇 첫 단위 구현**:
  - NPC의 skinned/morph mesh component에서 render proxy를 찾아 주소 라이브러리의
    `RenderProxy::SetHighlightParams`/`SetScanningState`를 호출하는 네이티브 through-wall highlight를
    추가. 기본값은 꺼짐이며 카테고리/사망 필터를 따르고, 토글 off와 DLL 언로드 때 현재 살아 있는 proxy를
    `ScanningState::Off`로 정리함. 라이브에서 함수 해석, 활성화, 안전 언로드 시 15개 엔티티 정리와 게임
    응답 유지를 확인함.
  - 높이나 자세 추정 대신 엔티티 component 목록의 `entCorpseComponent`를 RTTI 상속 체인으로 판별하는
    `Hide dead NPCs`(기본 on)를 추가하고 설정 저장에 포함. 현재 관측 구간에는 dead 대상이 없어 실제
    생존→사망 전환의 시각 검증은 다음 전투 테스트에 남아 있음.
  - 에임봇은 적/선택 시 경찰 중 FOV 내부 화면 중심에 가장 가까운 살아 있는 대상을 선택하고, 실제 AABB
    상단 12% 지점을 머리 조준점으로 사용(없으면 위치+1.62m fallback). 메뉴가 닫혀 있고 게임이 foreground인
    동안 우클릭을 누르면 프레임률 독립 smoothing과 소수점 누적을 거쳐 상대 `SendInput`으로 조준함.
    PID 21768에서 실제 target ID, screen delta, depth를 기록하는 `aimbot active` 로그와 게임 응답 유지를
    확인. silent aim, 실제 health/stat pool, 동적 hostility는 아직 미구현.
- **라이브 포즈 슬롯과 게임 네이티브 에임 오프셋으로 전환**:
  - REDmod 원본 스크립트에서 `entSlotComponent::GetSlotTransform`, `gameITargetingSystem::LookAt`/
    `BreakLookAt`, `AimRequest` 필드 구성을 확인하고, 함수 자체의 RTTI 파라미터 타입을 사용해 REDengine
    함수를 호출하는 공용 `rtti_invoker`를 추가함. 기존 MetaRig transform은 현재 애니메이션 포즈가 아니라
    T 포즈로 보이는 bind pose였으므로 스켈레톤 소스로 사용하지 않도록 제거함.
  - NPC의 `Head`, `Chest`, `Hips`, `RightHand`, `LegLeft`, `LegRight` 슬롯 월드 transform을 33ms 캐시로
    읽어 라이브 스켈레톤을 만들고, 실제 `Head` 위치를 에임 타겟으로 우선 사용함. PID 31280에서 실제 AABB
    5개와 슬롯 기반 skeleton line 25개가 지속 갱신되고, RTTI internal execute 해석 뒤 게임이 정상 응답함을
    확인함.
  - 마우스 상대 이동/`SendInput` 경로를 완전히 제거하고 TargetingSystem의 게임 내부 `LookAt` 요청으로
    교체함. 우클릭 동안 타겟을 유지하며 smoothing 0은 1ms duration/ease-in 없음으로 매 프레임 헤드 월드
    위치에 강력 고정하고, 0보다 크면 duration 기반 게임 네이티브 보간을 사용함. 라이브 로그에서 같은
    세션에 `mode=smooth`와 `mode=hard`가 실제 NPC 헤드 좌표로 호출되고 화면 오차가 수렴하는 것을 확인함.
  - 조사 중 Present 경로에서 런타임 타입명을 추가로 문자열화한 일회성 진단은 게임을 하드 프리즈시켜 즉시
    전부 제거했고, 재시작 후에는 숫자/포인터만 기록하는 최소 진단으로 검증함. `build-next` 후보와 정식
    `build/bin/Release` 전체 빌드, `git diff --check`, 게임 응답을 모두 통과함.
- **ESP 박스 축소(카메라 정면 박스)와 시야(가림) 검사 도입, 그리고 동기 physics 쿼리 프리즈 사고**:
  - 사용자가 (1) AABB 박스가 실루엣보다 크고 (2) visible check가 필요하다고 지적함.
  - 박스: 기존에는 애니메이션 시스템 AABB의 8꼭짓점을 투영해 화면 min/max를 잡았는데, 이 AABB는 무기·
    모션까지 감싸는 데다 월드 축 정렬이라 대상이 축과 어긋나면 항상 실루엣보다 넓어졌다. 이미 읽고 있던
    라이브 슬롯 포즈(Head/Chest/Hips/RightHand/LegLeft/LegRight)를 `VisualData::posePoints`로 보관하고,
    엔티티 중심 기준 수평 반경(+0.16m, 0.26~1.10m clamp)과 발밑~머리+0.14m 높이로 실린더를 만든 다음
    카메라-대상 방향에 수직인 축으로만 폭을 잡는 4점 투영으로 화면 사각형을 만들도록 교체함. 애니메이션
    AABB는 포즈 슬롯이 없을 때의 fallback으로만 남기고 수평 반경도 사람 크기로 제한함. PID 31280 라이브
    로그에서 화면 안 대상 전부가 포즈 기반(`poseBounds=10/10`)으로 잡히는 것을 확인함.
  - 카메라 월드 좌표: RedHotTools가 쓰는 `gameICameraSystem` vtable +0x218 `GetCameraPosition(Vector3&)`을
    1순위로 하고, 실패 시 RTTI `GetActiveCameraWorldTransform`을 대체 경로로 둠. 카메라 지점을 다시
    투영하면 전방 깊이가 0에 가까워야 한다는 성질로 자체 검증함. 2.31 실측에서 vtable 경로가 통과
    (`camera position source: virtual slot`).
  - 시야 검사: 게임 스크립트가 NPC 시야 판정에 쓰는 `gameISpatialQueriesSystem::SyncRaycastByQueryPreset`
    + `'Sight Blocker'` preset을 그대로 사용. RTTI 파라미터 수(6개)를 실행 시 검증해 시그니처가 다르면
    스스로 비활성화하도록 함. 라이브에서 `raycast=00007FF75614FDB0 params=6 preset=Sight Blocker`로
    해석되고 실제로 가림/비가림이 갈리는 것까지 확인함.
  - **사고**: 이 동기 레이캐스트를 Present(렌더) 스레드에서 호출했더니 약 19초/1,158캐스트 뒤 게임이 하드
    프리즈했다. 게임 스레드의 물리 스텝과 렌더 스레드가 서로를 기다리는 교착으로 보이며, `--unload`도
    콜백이 빠져나오지 못해 실패했다(프로세스 강제 종료 필요). **동기 physics/씬 쿼리는 Present 경로에서
    절대 호출하지 않는다**를 규칙으로 삼는다.
  - 재설계: 시야 검사를 전용 워커 스레드로 옮기고 `Query()`는 절대 블로킹하지 않도록 함. 렌더 스레드는
    캐시된 결과(없으면 Unknown=보이는 것으로 취급)를 즉시 쓰고 갱신 요청만 링버퍼에 넣는다. 워커는 wake
    이벤트당 최대 8회 캐스트, idle 10ms. 워커가 게임 호출 안에서 빠져나오지 못하면 `Shutdown()`이 false를
    반환하고 훅 종료를 중단해 코드가 언로드되지 않게 함.
  - 자기 몸 오탐 대응: 조준점이 대상 몸 안쪽이라 preset에 NPC 콜라이더가 있으면 자기 자신을 맞는다.
    고정 pull-back 대신 `physicsTraceResult.position`까지의 거리를 대상 거리와 비교해 0.55m 이내면
    가려지지 않은 것으로 판정하도록 바꿈.
  - 프리즈 이력 때문에 `esp.visibility_check`와 `aimbot.visible_only` 기본값은 꺼짐으로 두고 메뉴에서
    켜도록 함. 가려진 대상은 지우지 않고 알파를 낮춰 그리며, `hide_occluded`를 켜면 완전히 숨긴다.
  - 정식 `build/bin/Release` 전체 Release 빌드가 `/W4`에서도 경고 없이 통과. 재설계본의 라이브 검증은
    게임 재시작 후로 남아 있다.
  - **두 번째 사고 — 네이티브 하이라이트 크래시**: 위 재설계본을 주입한 뒤 사용자가 메뉴에서 `Civilians`를
    켜자마자 게임이 크래시했다. 원인은 오늘 작업분이 아니라 기존 `native highlight` 기능이었다.
    저장된 설정이 `native_highlight=1`, `show_civilians=0`이었고 해당 세션에 enemy/police가 0명이어서
    `shouldHighlight=true`가 된 적이 한 번도 없었다. 즉 시빌리언을 켠 그 프레임이 하이라이트 on 경로의
    최초 실행이었다. off 파라미터로는 20초 넘게 멀쩡했고 on 파라미터에서 죽었으며, 엔티티별 SEH로도
    잡히지 않아 렌더 상태를 깨뜨리는 지연 크래시로 보인다. 이 경로는 추정 오프셋(0x1E0/0x1E8)으로 얻은
    render proxy를 Present(렌더) 스레드에서 직접 조작한다.
  - RedHotTools를 다시 확인하니 **엔티티에 대해서는 render proxy를 직접 건드리지 않는다**:
    `entRenderHighlightEvent`를 만들어 `entity->QueueEvent(...)`로 넣고 게임 스레드가 처리하게 한다
    (`WorldInspector::SetEntityHighlightEffect`). proxy 직접 조작은 world node instance 전용이며
    노드 타입마다 오프셋이 다르다. 재구현 전까지 `UpdateNativeHighlights`는 진입 즉시 로그 한 줄만 남기고
    아무 것도 하지 않도록 막았고(`#if 0`으로 기존 경로 보존), 메뉴 라벨에도 비활성 상태를 표시했다.
    사용자 config는 임의로 고치지 않았다.
  - 정리된 규칙: **Present(렌더) 스레드에서 게임 서브시스템(physics 쿼리, render proxy)을 직접 호출하지
    않는다.** 필요한 일은 워커 스레드로 비동기화하거나 게임 자체 이벤트 큐에 넣는다.
- **Visibility 동기 physics 호출을 게임 메인 틱으로 이전하고 라이브 안정화 확인**:
  - 전용 워커 버전도 게임이 소유하지 않은 임의 스레드에서 REDengine의 동기 spatial query를 호출한다는
    근본 위험이 남아 있었다. 실제 워커 버전 세션은 약 4,561캐스트 뒤 `CrashInfo.json`을 남겼으므로,
    네이티브 LookAt 에임을 원인으로 단정하지 않고 visibility 실행 컨텍스트를 다시 조사했다.
  - CET/RED4ext의 실제 구현을 기준으로 CET가 `red::GameAppRunningState::OnTick` 주소 해시
    `3592689218`을 훅해 Lua `onUpdate`를 실행한다는 점을 확인했다. 같은 OnTick을 MinHook으로 체인 훅하고,
    visibility의 `SyncRaycastByQueryPreset`은 이 게임 메인 틱에서만 호출하도록 Luna worker가 구현했다.
    기존 worker/event/join 경로는 제거했다.
  - Present의 `Query()`는 캐시 조회와 bounded queue 등록만 수행한다. OnTick은 틱당 요청 1개를 꺼내
    1차 레이캐스트를 실행하고, 가려졌을 때만 보조 지점으로 최대 1회 더 검사한다. 캐시 갱신 간격은 500ms,
    큐 용량은 64이며, 게임 함수를 호출하는 동안 내부 mutex를 잡지 않는다. 종료 시 전체 훅을 먼저 끄고
    진입 중 콜백이 빠져나온 다음 visibility 상태를 정리한다.
  - PID 10908 라이브 검증에서 OnTick 주소 `0x00007FF7526233C4`, 원본 trampoline
    `0x00007FF751BB0F80`, 메인 틱 thread id `22364`를 확인했고 spatial query resolver가 같은 틱에서
    성공했다. 이동 중 snapshot이 18~32개로 갱신되고 enemy/police/civilian 등 새 대상이 계속 들어왔으며,
    visibility의 clear/blocked 카운터가 모두 증가하고 ESP와 native LookAt 에임이 동시에 작동했다.
    사용자가 실제 이동 및 안정화를 확인했고 프로세스는 계속 응답했으며, `CrashInfo.json` 시각도 이전
    크래시인 19:23:29에서 바뀌지 않았다.
  - 대상 변동이 큰 구간에는 bounded queue의 drop 누계가 약 95,939까지 증가했지만 큐는 이후 0까지
    정상적으로 소진됐고 프리즈나 크래시는 발생하지 않았다. 이는 안전한 backpressure로 동작하지만,
    중복 요청 제거와 갱신 스케줄 최적화는 후속 성능 개선 후보로 남긴다.
  - 최종적으로 `inject.py --unload` 안전 종료도 통과했다. 로그에서 훅 비활성화 뒤 visibility 상태가
    `casts=25423 visible=4541 occluded=10279 dropped=95939`로 정리되고 `hook shutdown finished`,
    `safe unload checks passed` 순서가 기록됐으며, DLL 모듈이 빠진 뒤에도 게임 프로세스는 응답 상태였다.
- **스트리밍 ESP, 네이티브 하이라이트, 체력/사망, hard aim, no-recoil 통합 구현 및 라이브 검증**:
  - 기존 `RegisterEntity` 콜백은 transform이 아직 준비되지 않은 NPC를 추적 목록에 넣지 않았고,
    snapshot 실패도 임시 transform 부재와 stale pointer를 구분하지 않고 삭제했다. 모든 NPC를 먼저
    추적하고 snapshot 결과를 `Ready/PendingPosition/Stale`로 분리했다. 라이브 이동/스트리밍에서 추적 수가
    `0 -> 1 -> 21 -> 26`으로 증가했고, transform 대기 항목은 보존되며 실제 stale 1개만 제거됐다.
    `UnregisterEntity` 주소 해시는 확보했지만 네이티브 ABI를 검증하지 못했으므로 추정 시그니처 훅은 설치하지
    않고 ID/type 재검증 경로를 authoritative cleanup으로 유지했다.
  - RedHotTools commit `b4d341527bce19842d16a757028be901d4a2d6a8`의
    `WorldInspector::SetEntityHighlightEffect`를 기준으로 `entRenderHighlightEvent`(0x58)를 만들고
    `QueueEvent`로 전달하는 네이티브 ESP를 복구했다. 이벤트 생성, VisionModeSystem 전환, QueueEvent는 전부
    기존 게임 `OnTick` 훅에서만 실행하고 Present는 atomic 설정만 게시한다. 과거의 추정 render-proxy
    `0x1E0/0x1E8` 직접 호출 경로는 제거했다.
  - 첫 라이브 검증에서 활성 대상 조건식이 매 틱 이벤트를 다시 보내 `queued=4096` 상한까지 차는 버그를
    발견했다. unknown->enabled 또는 실제 상태 변경 때만 큐잉하도록 수정하고, 최대 256개 clear용 이벤트
    headroom을 예약했다. 수정본은 NPC 8/9/10명에서 queued가 8/9/10으로 고정됐고, 1명 사망 시
    `queued=11 cleared=1 failures=0`으로 정확히 한 번 clear됐다. 안전 종료 때 남은 활성 엔트리도 모두
    clear한 뒤 Braindance mode를 0으로 내리고 다음 틱에서 acknowledgement한다.
  - 게임 REDmod의 `StatPoolsSystem` API를 기준으로 Health pool(17)의 현재/최대/최솟값 도달 여부를 메인
    틱에서 bounded round-robin으로 읽어 캐시했다. health bar는 이 비율을 box 왼쪽에 렌더하고, stat pool이
    유효할 때 사망 판정도 같은 값으로 수행한다. 라이브에서 health resolver 3종이 성공하고 valid 값이 계속
    증가했으며 invalid 0인 구간을 확인했다. 실제 처치 후 진단이 `dead=0 -> dead=1`로 바뀌고 hide-dead가
    적용됐다.
  - smoothing 0은 `duration/maxDuration/precision=0`, ease off, `processAsInput=false`, time limit off인
    별도 hard request로 변경했다. 새로 스트리밍된 적에서 hard mode가 실행되고 마지막 screen delta가
    `(0.1, 0.2)`까지 수렴하는 것을 확인했다.
  - no-recoil은 Simple Menu의 장착 무기 StatsSystem multiplier 방식과 RED4ext SDK stat enum을 기준으로
    RecoilAngle/Dir/KickMin/KickMax/AlternateDir의 hip/ADS 항목 및 ADS 분리 플래그 11개에 0 multiplier를
    적용한다. 실제 게임에서 TransactionSystem이 장착 무기 ID를 찾고 modifier 11개 적용/무기 교체 시 11개
    제거 후 새 무기에 재적용하는 로그를 확인했다. API가 정상인데 무기가 없는 경우 player ID에 잘못
    fallback하던 초안은 제거하고 재장착까지 대기한다.
  - 최종 Release 빌드와 `git diff --check`를 통과했다. PID 10908에서 네이티브 ESP와 no-recoil이 활성인
    상태로 안전 종료했으며 `native highlight cleanup acknowledged: queued=22 cleared=11`,
    `no-recoil modifiers removed: count=11`, `safe unload checks passed`가 순서대로 기록됐다. 게임은 계속
    응답했고 `CrashInfo.json`은 이전 크래시 시각 19:23:29에서 변하지 않았다. 테스트용 사용자 설정
    `no_recoil=1`은 종료 후 다시 0으로 복구했다. 이벤트 handle의 완전한 소멸 ABI는 추정하지 않고 4096개
    bounded 보수적 수명 정책을 유지하며, 정상 전이 전용 큐잉으로 일반 세션에서는 매우 천천히 증가한다.
- **Native LookAt을 제거하고 FPPCameraComponent 직접 메모리 에임으로 전환**:
  - smoothing 0에서도 체감 Ease가 남는다는 인게임 피드백을 받아 `TargetingSystem::LookAt` 요청 필드 조정으로는
    해결할 수 없다고 판단했다. PID 10908의 `gameCameraSystem`과 로컬 플레이어 component를 실메모리에서
    대조해 활성 카메라 quaternion이 `cameraSystem+0x70` 및 `gameFPPCameraComponent+0xF0`에 있고, 실제 입력
    yaw/pitch 상태가 FPPCameraComponent `+0x42C/+0x430`에 있음을 확인했다.
  - 두 필드의 writer/reader xref를 디스어셈블해 `Cyberpunk2077.exe+0x4E2968`의 provider-read helper가
    FPPCameraComponent 업데이트 초기에 yaw/pitch 포인터를 채운다는 것을 확인했다. 2.31 실행 파일에서 40바이트
    AOB가 정확히 1개만 매치하며, MinHook으로 원본 provider read 직후 두 값을 덮어쓰도록 구현했다. 이로써
    `AimRequest`, `LookAt`, `BreakLookAt` 경로를 전부 제거했다.
  - 라이브 조사 중 camera state에 따라 yaw 기준축이 절대/상대 방식으로 바뀌고, pitch limit 필드도 현재 유효
    offset과 일치하지 않는 상태가 있음을 자체 layout check가 검출했다(초안 오차 yaw 50.797도, 다른 상태의
    pitch clamp 오차 29.469도). 고정 body quaternion이나 동적 min/max를 가정하지 않고, 매 카메라 틱의 현재
    world quaternion과 현재 offset의 차이에 목표 world angle delta만 더하는 방식으로 수정했다. 최종 live
    check는 `current=(-15.000,0.000) resolved=(-15.000,0.000) delta=(-0.000,0.000)`으로 통과했다.
  - smoothing 0은 hook 안에서 계산된 yaw/pitch를 같은 틱에 그대로 쓰며 엔진 easing을 전혀 거치지 않는다.
    0보다 큰 값만 trainer 자체의 frame-time 기반 지수 보간을 사용한다. Present→camera tick 대상 전달은
    odd/even generation seqlock으로 찢어진 좌표를 방지하고, 훅 콜백은 공용 unload lifecycle guard에 포함했다.
  - Release 빌드와 `git diff --check`, 여러 차례 안전 언로드/재주입을 통과했다. 최종 주입 로그에서 AOB 1개,
    hook callback 유입(`callbacks=3`), 로컬 FPP camera 해석, 0도 layout 오차를 확인했고 게임은 계속 응답했다.
    현재 스트림에 enemy가 다시 잡혔지만 실제 우클릭 hard-lock의 시각적 체감 확인은 사용자 피드백을 기다린다.
- **직접 메모리 에임 후속 조사 — derived projection cache 폐기 및 FPP 입력 ABI 복구**:
  - `+0x4E2968` provider-read helper 뒤에서 `FPPCameraComponent+0x42C/+0x430`을 덮는 구현은 콜백/쓰기
    카운터가 증가해도 실제 화면을 움직이지 않았다. 우클릭 유지 중 22:32:26 크래시도 발생했으므로 해당 필드는
    최종 카메라 소스가 아닌 provider 상태 복사본으로 판정하고 훅을 제거했다.
  - `gameICameraSystem+0x70` quaternion과 `+0xB0..+0x190` 회전 행렬을 post-main-tick에서 바꾸는 실험은 실제
    화면은 그대로인데 ESP만 화면 중앙으로 이동시켰다. `ProjectPoint`가 이 행렬들을 읽는 것을 디스어셈블로
    확인했으며, 이들은 렌더 카메라 원본이 아니라 CPU world-to-screen 투영 캐시이므로 해당 쓰기 경로도 폐기했다.
  - `gameFPPCameraComponent::Update`(`Cyberpunk2077.exe+0x4E1638`)가 입력 객체
    `component+0x360 -> +0x44/+0x60`에 yaw/pitch를 기록하는 것을 확인했다. 첫 임시 detour는 함수의 스택 인자
    3개를 누락해 4인자 ABI로 원본을 호출했고, 그 결과 additive-input 값이 오염되어 카메라가 180도 회전하고
    상하 입력이 잠겼다. 즉시 안전 언로드한 뒤 실제 7인자 ABI
    `(camera, dt, yaw, pitch, additiveYaw, additivePitch, hasAdditiveInput)`로 수정했다.
  - 오염 상태는 component pitch가 `NaN`, yaw 한계가 `+89/-89`, pitch 한계가 거의 `0/0`, pitch 보조값이
    약 `1.54e11`인 것으로 실메모리에서 확인했다. 같은 세션 초기에 캡처한 정상값을 기준으로 component 원본
    한계(yaw `+180/-180`, pitch `-80/+80`)와 내부 입력 객체, 현재/이전 pitch 값을 복원했다. 14초 뒤 재검사에도
    정상값이 유지됐고 새 플레이어 인스턴스에서 body/FPP/render yaw가 모두 `-8.106`도로 일치했으며 게임은 응답했다.
  - 수정된 7인자 detour의 진단 프로브에서는 상태 손상이나 지속 회전이 재현되지 않았다. 세 번째 yaw 입력 인자
    자체는 작은 값에서 렌더 회전을 만들지 않아, 실제 소비되는 입력 경로와 배율은 추가 확인이 필요하다. 자동
    프로브 코드는 소스에서 제거했고 검증 전 DLL도 안전 언로드했다. 이 중간 상태는 아직 커밋하지 않는다.
- **카메라 컨트롤러의 실제 yaw 입력 필드 확보 및 Cyberpunk 자동 인젝터 추가**:
  - 7인자 FPP 업데이트 입력을 한 인자씩 자동 격리했다. 네 번째 인자 `+0.1`은 pitch를 약 `+0.106`도
    움직였지만 세 번째 인자는 지연 측정에서도 yaw를 만들지 않았다. 다섯/여섯 번째 인자도 pitch/additive
    보정 계열이었다. 따라서 FPP 함수는 pitch를 소비하지만 수평 회전은 바깥 컨트롤러가 소유한다고 판정했다.
  - FPP 호출부 `Cyberpunk2077.exe+0x3F32A4`를 디스어셈블해 호출 직전 `FPPCamera+0x4E4`, 내부 입력
    `+0x4C`, 별도 컨트롤러 입력 객체 `+0x9C`에 같은 yaw 값을 쓰는 흐름을 확인했다. 앞의 두 캐시를 정확한
    타이밍에 덮어도 카메라 변화는 `0.000031`도뿐이었지만 컨트롤러 `+0x9C`에 `0.1`을 6틱 넣은 자동
    프로브는 실제 렌더 카메라를 `+0.601364`도 움직였다. 이 필드를 1:1 권위 yaw 입력으로 확정했다.
  - 플레이어 루트 `IPlacedComponent::SetTransform`도 주소 해시 `1828854026`와 2.31 주소
    `Cyberpunk2077.exe+0x574FC8`을 확인해 1도 왕복 시험했으나 카메라 변화가 `-0.006577`도에 그쳤다.
    직접 quaternion 캐시 쓰기와 마찬가지로 컨트롤러에 덮이는 경로라 판정하고 최종 코드에서 전부 제거했다.
  - 최종 메모리 에임은 카메라 컨트롤러 업데이트를 66바이트 고유 AOB로 후킹해 현재 호출의 입력 소유자를
    thread-local로 전달하고, 중첩 FPP 업데이트에서 yaw는 `controllerInput+0x9C`, pitch는 검증된 네 번째
    인자로 같은 각도 delta를 기록한다. smoothing 0은 두 축 모두 `alpha=1`이라 엔진 LookAt/AimRequest와
    trainer 보간을 거치지 않는다. 진단용 자동 프로브와 실패한 SetTransform 코드는 모두 제거했다.
  - `tools/scripts/inject.py --auto`를 추가했다. 기본적으로 `Cyberpunk2077.exe`를 감시하고 Release DLL을
    PID당 한 번만 주입하며, 안전 언로드 후 같은 PID에는 재주입하지 않고 게임 재시작으로 PID가 바뀌면 다시
    주입한다. `--once`, `--dry-run`, `--interval`, DLL 경로 override를 지원하고 더블클릭용
    `tools/scripts/auto_inject_cp2077.cmd`도 추가했다. 문법 검사, help, 실행 중 PID dry-run 검사를 통과했다.
  - 최종 Release 빌드에서 FPP 및 컨트롤러 AOB가 각각 정확히 1개 매치했고 로컬 player/FPP camera 해석과
    게임 응답 상태를 확인했다. 실제 우클 타겟 추적 로그는 아직 발생하지 않았지만, yaw/pitch 각 입력 경로는
    별도의 자동 실메모리 프로브로 화면 반응을 검증했다.
## 2026-08-28 - Silent-aim weapon event listener path rejected

- Live combat confirmed the original projectile-component listener does not receive ordinary gunfire: target
  selection armed correctly, but projectile/weapon/redirect counters remained zero.
- A read-only `Entity::QueueEvent` observer was stable and processed hundreds of thousands of events, but event ID
  108 (`gameweaponeventsShootEvent`) never traversed that path. The observer was removed from normal startup.
- Full RTTI enumeration found exactly two native listener entries for event ID 108. Hooking those raw callback code
  targets was not safe: the targets are reused outside the typed listener dispatch. Treating their second argument as
  either an `IScriptable` event or the known 0x1E0 payload never produced a valid payload, and the latter experiment
  generated a new 2.31 crash at 01:09:51 before any payload log was written.
- Weapon-listener observation hooks and all silent-aim mutation remain disabled. Do not re-enable these raw callback
  target hooks. Future work must hook a call site that still carries explicit event metadata or the weapon fire/
  ballistic producer itself.

## 2026-08-28 - Silent-aim producer-path observation instrumentation

- The shipped REDmod scripts confirm that hitscan attacks use the `gameEffectInstance` shared-data pipeline. The
  native RTTI names differ from their REDscript aliases: `gameEffectInstance`, `gameAttack_GameEffect`, and
  `gametargetingTargetingSystem`. `StartAttack` is inherited from `gameIAttack`; `PrepareAttack` belongs to
  `gameAttack_GameEffect`.
- Extended the RTTI bridge with CName-pool reverse lookup and native function metadata inspection. The public
  RED4ext SDK describes native instance dispatch as `CBaseFunction_Handlers[regIndex]`, but its relocation resolves
  to the table base on 2.31. Read-only disassembly of `rtti::Function::InternalCallNative` at
  `Cyberpunk2077.exe+0x14619C` confirmed the direct `base + 0x33E6E50 + regIndex * 8` lookup; an extra pointer
  dereference incorrectly produced null handlers and was removed.
- Added observation-only hooks for five uniform RTTI VM handlers. Live 2.31 resolution produced unique targets:
  `EffectInstance::Run` at `+0x1070334`, inherited `IAttack::StartAttack` at `+0x1D90FE0`,
  `Attack_GameEffect::PrepareAttack` at `+0x1D912B0`, `TargetingSystem::GetCrosshairData` at `+0x2512ACC`, and
  `GetDefaultCrosshairData` at `+0x2512D70`. The detours use the documented native handler ABI, participate in the
  unload callback guard, immediately pass through when no fresh target is armed, and only increment/log counters.
  No stack-frame fields, effect data, crosshair outputs, or projectile data are modified.
- CName reverse lookup identified the two event-108 subscribers as `gameuiCrosshairContainerController` and
  `gameWeaponAudioComponent` (callback CName `function`). Their unsafe raw targets remain enumeration-only and are
  never hooked. Startup diagnostics confirmed `producers=5`, `projectileListeners=1`, `weaponListenerHooks=0`,
  `queueHook=0`, and `mutation=0`.
- Release build and `git diff --check` passed. Live injection into PID 17276 created all five producer hooks without
  exceptions and the game remained responsive. Safe unload removed the DLL with all producer counters still zero
  because no armed shot was fired during this startup-only validation. The next live test must hold the configured
  activation key with a valid target and correlate one ordinary shot against the five counters before any mutation
  is enabled.

## 2026-08-28 - Silent-aim producer classification Step 1a (shared-data reflected probe)

- Live combat with the producer instrumentation showed only `EffectInstance::Run` fires on ordinary gunfire
  (0 -> 6 within ~240 ms of a shot), while `StartAttack`/`PrepareAttack`/`GetCrosshairData`/`GetDefaultCrosshairData`
  and the projectile listeners stayed at 0. This confirms native hitscan bypasses the script attack-prep helpers and
  is consistent with the shipped REDmod recipes (`tankTurret`, `damageSystem` ricochet) driving the shot through the
  `gameEffectInstance` shared-data pipeline. It does NOT yet prove any of those 6 Run calls is the ballistic/damage
  effect: a single hitscan shot spawns several cosmetic effects (muzzle flash, tracer, casing, audio, impact) that
  also route through `Run`, and many are script-driven (`baseBullet.OnShoot`). Classification of the 6 is required.
- The producer hook logged the Run handler `context` type as `?(0)` because `NativeType()` reads the CClass at
  `+0x30`, which holds the type only for heavy game entities; `gameEffectInstance` is a lightweight native
  `IScriptable` whose type comes from its vtable `GetType()`, not `+0x30`. So the null type is a NativeType-path
  artifact, not evidence the context is wrong. Do not apply guessed offsets to the Run context.
- The shipped `core/gameplay/gameplayEffects.script` exposes a fully reflected path that needs no internal-function
  ABI and no IDA: `gameEffectInstance::GetSharedData() : EffectData`, plus the `EffectData` statics
  `GetVariant/GetBool/GetVector`. The `EffectSharedDataDef` blackboard (`core/blackboard/blackboardDefinitions.script`)
  carries `attack` (variant, identifies a real combat attack), `playerOwnedWeapon` (bool), `position`,
  `muzzlePosition`, `forward`, and `raycastEnd` (the game itself sets `raycastEnd` to an enemy chest slot to force a
  hit in `triggerAttackOnNearbyEnemiesEffector`). This is the same reflected-invocation methodology already proven
  for LookAt/StatPools, not offset guessing.
- Implemented Step 1a as an observation-only reflected probe (`kEnableSharedDataProbe`, mutation stays 0). When an
  `EffectInstance::Run` handler fires while a target is armed, it validates the context vtable is inside the
  executable, then invokes the 0-argument const getter `GetSharedData()` via the existing `rtti::Invoke`, into a
  64-byte over-provisioned result buffer (covers a pointer or a small struct return with no stack risk). It is
  reentrancy-guarded (thread-local) with `__try/__finally` reset, bounded to 256 attempts, and logs the first 16
  results (`invoked`, `valid`, `shared` handle). Startup resolves the getter once and disables the probe unless it is
  a 0-parameter function. This is the low-risk increment that settles the central unknown (is `context` really an
  EffectInstance, and is a reflected call from inside the handler stable) before any field reads or mutation.
- Release build passed with no warnings (only `silent_aim.cpp` recompiled). Injected into PID 17276 (RED4ext present,
  our DLL not previously loaded): `shared-data getter: params=0 hasReturn=1`, `producers=5`, no exceptions, process
  `Responding=True`.
- **Live result: the probe answered Step 1a's question YES, then froze the game.** During combat it fired
  `invoked=1 valid=1` with valid `shared` handles (0x27E4xxxx range) and a consistent context vtable
  `0x7FF75474E6B8` across every call, proving the Run handler `context` is a real `gameEffectInstance` and that
  `GetSharedData()` returns valid shared data. But the probes ran on many concurrent game threads (e.g. tid 30100 and
  31432 both at `01:56:38.540`), and after 8 successful probes two threads reentering the script VM
  (`InternalExecute`) inside the Run dispatch simultaneously deadlocked the game (`Responding=False`, hung — like the
  earlier synchronous spatial-query freeze; `--unload` cannot recover because the hooked callback never returns, so
  the process must be force-killed).
- **Rule reaffirmed / new constraint:** never call a synchronous REDengine or script-VM function from inside a hot
  native handler on arbitrary game threads. `kEnableSharedDataProbe` is set to `false`; the in-handler reflected call
  must not be re-enabled. The plain producer counters (no VM call) remain safe as previously verified.
- **What Step 1b must change:** we already have the confirmed EffectInstance context and a way to obtain the shared
  data, but not from inside Run concurrently. Options to evaluate next (all still avoiding IDA): (a) serialize probes
  through a single global gate so at most one thread ever reflects, and cap to a handful of one-shot captures rather
  than every Run; (b) capture the raw `context`/`shared` pointer during Run and read `playerOwnedWeapon`/`attack`/
  `position`/`forward` directly from the EffectData/blackboard memory layout without the VM; (c) reconsider whether a
  lower-frequency, single-threaded producer exists. The safe rebuilt DLL (probe off) is ready to reinject once the
  frozen PID 17276 is restarted.

## 2026-08-28 - RED4ext.SDK EffectInstance/EffectData layout inspection (Step 1b prep)

- Inspected the RED4ext.SDK generated headers to see how much of the shared-data layout the public SDK already
  gives us before writing any Step 1b code. Result: the SDK is fully opaque for both types, so field offsets
  (`raycastEnd`/`forward`/`position`/`muzzlePosition`/`playerOwnedWeapon`) must be reverse-mapped live, not read
  from the SDK.
  - `game::EffectInstance` (`gameEffectInstance`) inherits `game::IEffect`, `RED4EXT_ASSERT_SIZE = 0x5AB0`, and its
    only body is an opaque `uint8_t unk40[0x5AB0 - 0x40]`. No named fields. This is consistent with Step 1a's
    confirmed context being a large native IScriptable.
  - `game::EffectData` (`gameEffectData`) is only `RED4EXT_ASSERT_SIZE = 0x8` with a single opaque `unk00[0x8]`.
    That means the value returned by `GetSharedData()` is an 8-byte handle/pointer to the shared blackboard, not an
    inline struct — matching Step 1a's `GetSharedData()` returning a pointer-sized `shared` handle. Reads must
    dereference this handle, not treat the 8 bytes as the data itself.
  - Takeaway for Step 1b: the reflected `EffectData` statics (`GetVariant`/`GetBool`/`GetVector`) are the only
    documented way to read named fields; the raw byte offsets behind the handle have to be recovered by comparing
    a reflected value against raw memory on the game main tick (same reverse-mapping method used for the
    `isCivilian/isPolice/isGanger` property offsets), because the SDK exposes none of them.

## 2026-08-28 - Silent aim landed via native crosshair core direction redirect

- An experimental combined build (7 producer hooks, `raycastEnd` effect-ray mutation through shared-data slot
  probing, plus a first native-crosshair-core redirect with verbose per-call logging) fired both mutation paths in
  live combat at 05:08 (`redirected effect ray` count=2, crosshair redirects count=4), then the game crashed at
  05:08:51 (`CrashInfo.json`, player position matching the logged shot origin, session 587 s). The crash could not
  be attributed to a single path because the effect-ray mutation, raw shared-data probing on game threads, and the
  crosshair hook were all active simultaneously. That combined build was never committed.
- The source was pared down to a single mutation as the isolation step: hook only the native crosshair core.
  A 37-byte AOB (1 exact match in 2.31) finds the `TargetingSystem::GetCrosshairData` native wrapper at
  `Cyberpunk2077.exe+0x2A60F44`; following the `call` at wrapper `+0x3C` resolves the shared core at
  `Cyberpunk2077.exe+0x4D8354` used by ordinary firearm shots. The MinHook detour calls the original first, then — only while a fresh armed
  target exists — rewrites the caller-visible `direction` vector (normalized `target - origin`, SEH-guarded,
  finite/length-checked). All effect-ray mutation, slot probing, and verbose experiments were removed; the five
  producer hooks stay observation-only. Diagnostics now include the armed target's cached health/dead state.
- Live validation on PID 2620 (injected 07:19, combat 07:26): while armed, `nativeCrosshairCoreRedirects` grew
  0 -> 1,500+ with `rejected=0`, armed targets' health dropped in step with fire (e.g. 158.67 -> 46.33 within 2 s;
  several other targets driven to sub-half health), and the user visually confirmed off-crosshair shots landing on
  the armed target. No crash, no freeze, game responsive throughout; the earlier `CrashInfo.json` timestamp
  (05:08:51) did not change. Conclusion: the crosshair-core `direction` out-param alone steers real hitscan
  ballistics on 2.31 — no shared-data/effect mutation is needed for silent aim.
- Notes for follow-up: the core is called continuously (~70/s during combat), not per shot, so redirects while
  armed are frequent by design; one armed target reported `healthValid=0` (stat pool not yet resolved) and one had
  4,343 max health (likely a vehicle/boss-class puppet) — target filtering may want a health-pool validity check
  later. Crash-cause isolation for the retired effect-ray path is moot unless that path is ever needed again.

## 2026-08-28 - Silent aim menu promotion, bindable activation key, prioritized visibility

- **visible-only는 이미 두 모드 공유 경로에 있었다.** `Aimbot::RunFrame`의 타겟 선택 루프가 클래식/사일런트
  분기보다 앞에 있어서 `aimbot.visibleOnly` 필터는 사일런트 에임에도 그대로 적용되고 있었다(`dd6f530`부터).
  남아 있던 실제 구멍은 지연이었다: 시야 요청은 게임 메인 틱당 1건만 처리되는데(`kRequestsPerTick = 1`)
  ESP가 켜져 있으면 화면의 모든 NPC 요청이 같은 64칸 큐를 먼저 채워서, 정작 조준 중인 대상의 가림 판정이
  최대 1초까지 밀릴 수 있었다.
  - `Game::Visibility::Query`에 `priority` 인자를 추가해 큐 머리 앞쪽에 넣도록 했다. 큐가 가득 차면 기존과
    똑같이 버리기 때문에 `pending`으로 영영 남는 항목은 생기지 않는다.
  - 에임봇은 FOV 서클 안에 있거나 이미 잠긴 대상만 우선 요청으로 넣는다. 화면 거리 계산을 시야 검사보다
    앞으로 옮겨서 그 판단을 할 수 있게 순서만 바꿨고, 가려진 대상을 후보에서 빼는 동작과 캐시가 비었을 때
    통과시키는 fail-open은 그대로다.
- **사일런트 에임을 진단 모드에서 정식 메뉴 항목으로 승격.** 지금까지는 `aimbot.silentAim`이 켜지면
  `Overlay::OnPresent`가 무조건 headless 경로로 빠져서 오버레이를 한 프레임도 그리지 않았다. 그런데 WndProc
  훅은 오버레이 초기화 경로에서만 설치되므로, 설정 파일에 `silent_aim=1`이 저장된 상태로 주입하면 메뉴도
  Insert 키도 죽어서 되돌릴 방법이 없었다(설정 파일 수동 편집 외에는).
  - headless를 `aimbot.headlessDiagnostics`라는 별도 진단 토글로 분리하고, **오버레이가 한 번 초기화된 뒤,
    메뉴가 닫혀 있을 때만** 적용하도록 했다. 이제 켜둔 채로도 Insert로 언제든 메뉴를 다시 열 수 있다.
    DRED page fault 재현/격리가 필요하면 이 토글을 쓰면 된다.
  - 반대로 사일런트 에임을 켠 상태에서는 이제 오버레이가 정상적으로 그려진다 — 즉 07:26 라이브 검증 때와
    달리 ESP/FOV 서클 드로우가 같이 돌아간다. 렌더 쪽 크래시가 다시 나오면 먼저 이 토글로 격리할 것.
- **활성화 키 바인딩.** `VK_RBUTTON` 하드코딩을 `aimbot.activationKey`(가상 키 코드, 기본 0x02 = 마우스 우클릭)로
  바꾸고 `config.ini`의 `aimbot/activation_key`로 저장한다. 메뉴에는 누르면 다음 키를 잡는 바인딩 버튼을 추가했다.
  모든 키가 한 번 떼어진 뒤부터 입력을 받아서 버튼을 누른 클릭 자체가 바인딩되지 않고, Escape는 취소,
  Insert(메뉴)/End(언로드)는 후보에서 제외한다. 키 이름표는 직접 만든 ASCII 표를 쓴다 — `GetKeyNameTextA`는
  현재 레이아웃의 ANSI 코드페이지(cp949) 문자열을 돌려줘서 UTF-8을 기대하는 ImGui에서 깨진다.
- **메뉴 정리.** 에임봇 카드에 활성화 키 행과 현재 모드 기준 안내문(잡은 키 이름이 그대로 들어간다)을 넣고,
  `mutation is disabled` / `mutation off` 같은 사실과 다른 문구를 걷어냈다(크로스헤어 코어 리다이렉트는 이미
  실동작 중이다). 프로듀서 카운터 덤프는 `Silent aim diagnostics` 접이식 헤더 안으로 넣고, 항상 보이는 줄은
  시야 캐시 상태(visible/occluded/dropped)와 사일런트 에임 상태(후크 여부/리다이렉트/거부) 두 줄만 남겼다.
  `DiagnosticsSnapshot`에 `crosshairCoreHookCreated`를 추가해 UI가 후크 성공 여부를 직접 읽는다.
- Release 빌드는 `/W4`에서 경고 없이 통과했고 `git diff --check`도 깨끗하다. 라이브 검증은 아직 —
  이전 세션의 PID 2620이 옛 DLL을 물고 있어 `build/bin/Release/cp2077_trainer.dll` 링크가 막혀 있었고
  (`build-next`로 빌드해 확인), 메뉴/키 바인딩/사일런트 조준은 사람이 직접 조작해야 확인되는 항목이다.
  다음 세션에서 `--unload` 후 재빌드·재주입해 (1) 사일런트 에임 켠 상태로 메뉴가 뜨는지, (2) 바인딩한 키로
  조준이 걸리는지, (3) visible-only를 켰을 때 엄폐 중인 적이 후보에서 빠지는지 확인할 것.

## 2026-08-28 - Target quality filters and the first non-diagnostic silent aim build

- **관측 모드 종료.** 사일런트 에임이 크로스헤어 코어 리다이렉트만으로 동작하는 것이 확인됐으므로, Step 1
  진단용이던 후크들을 기본 빌드에서 뺐다. `kEnableProducerObservationHooks = false`,
  새로 추가한 `kEnableProjectileObservationHooks = false`. 이제 설치되는 후크는 크로스헤어 코어 하나뿐이다
  (라이브 로그: `producers=0 projectileListeners=0 weaponListenerHooks=0 queueHook=0 crosshairCore=1`).
  RTTI 조회/열거와 로깅은 시작 시 한 번만 도는 읽기 전용이라 그대로 뒀다. 다시 조사할 일이 생기면 두 상수만
  되돌리면 된다.
- **타겟 품질 필터.** 07:26 로그에서 `healthValid=0`인 대상과 최대 체력 4,343짜리 대상이 armed된 사례가
  있었다. 둘 다 타겟 선택 단계에서 걸러낸다 (클래식/사일런트 공통 경로).
  - `requireHealthPool`(기본 켜짐): 스탯 풀이 아직 안 잡혔거나(`healthValid=0`) 현재 체력이 0 이하인 대상 제외.
    살아 있는지조차 확인되지 않은 대상에 탄도를 돌리지 않는다.
  - `limitHealthPool`(기본 켜짐) + `maxHealthPool`(기본 2500, 500~6000): 최대 체력이 일반 NPC 범위를 크게
    벗어나는 차량/보스급 퍼펫 제외. 실제 적이 걸러지면 슬라이더를 올리면 된다. 두 값 모두 `config.ini`에 저장된다.
  - `Aimbot::GetStats()`로 프레임별 후보/통과/제외 사유(no pool, over cap, occluded) 카운트와 현재 선택된
    대상의 체력을 노출해 메뉴와 사일런트 에임 로그에 함께 찍는다. 필터가 실제로 무엇을 걸러내는지 눈으로
    확인하지 않으면 임계값을 조정할 근거가 없기 때문이다.
- **FOV 반경 상한 40~600 -> 40~2500.** 사용자가 손으로 `fov_radius_pixels=2500`을 넣어둔 상태였는데, 로드
  시 클램프가 600으로 깎고 그대로 저장해버린다. 2560x1440에서 화면 전체를 덮는 FOV는 사일런트 에임에서
  정상적인 설정이라 상한을 올려 설정을 보존했다.
- **라이브 검증 (PID 2620, 07:49 재주입).** `--unload`로 이전 DLL을 안전 언로드한 뒤 `build/`로 재빌드해
  재주입했다. 확인된 것:
  - `silent_aim=1`인 상태에서 `first overlay frame submitted`가 찍혔다 — headless 분리 이후 사일런트 에임과
    오버레이가 동시에 살아 있는 첫 빌드다. 5초 창 기준 presents 2671/3402/3378에 `submitted`가 동일하고
    `allocatorMisses=0`, device removal/펜스 타임아웃 로그 없음, 게임 `Responding=True` 유지.
  - 크로스헤어 코어 후크 생성 성공(`target=00007FF752108354`), 시야 리졸버(`preset=Sight Blocker`),
    메모리 에임 훅, 프로젝션 모두 정상 초기화.
  - 다만 재주입 시점의 세이브 위치에 NPC가 없어(`snapshots=0 tracked=0`) 체력 필터/시야 필터/키 바인딩은
    전투 중 사람이 직접 확인해야 한다. 다음 전투에서 메뉴의 `Targets: candidates/eligible/no pool/over cap/
    occluded` 줄과 `silent aim armed:` 로그의 같은 카운터를 비교할 것.
- Release 빌드는 `/W4`에서 경고 없이 통과, `git diff --check` 깨끗.

## 2026-08-28 - 07:52 GPU 크래시: 근거 재검토와 DRED 실측 준비

- **07:52:55.77 크래시는 GPU device removal이다.** RED4ext 로그에 게임 자체 핸들러
  `gpuApiDX12Error.cpp:42`("Gpu Crash for unknown reasons... Check if Breadcrumbs or Aftermath logged
  anything useful")가 남았고, 우리 쪽 CPU 예외 콜스택은 없다. 우리 로그의 마지막 줄은 07:52:55.649
  `config saved`이며, 그 직전 07:52:51에 세이브 로드(메가빌딩 H8), 07:52:55.1에 no-recoil 토글
  (`path=waiting-for-equipped-weapon`이라 게임 상태 변경 없음)이 있었다.
- **"오버레이가 원인"은 아직 확립된 사실이 아니다 — 기록을 검증한 결과 근거가 없었다.**
  `overlay.cpp`의 "DRED가 두 번 page fault를 보고했다"는 주석(`979eb10`에서 도입)을 append 모드로
  누적된 5.5MB 로그 전체와 대조했다. 실제로 있는 것은 `overlay fence reports device removal` 두 줄뿐이다:
  00:20:15.606 `hr=0x887A0006`(DEVICE_HUNG), 02:42:53.301 `hr=0x887A0001`. **DRED breadcrumb/page fault
  줄은 단 한 줄도 없다.** 애초에 DRED는 디바이스 생성 전에 켜져야 데이터가 채워지는데 우리는 주입 시점이
  디바이스 생성 이후라 켤 수 없다. 즉 페이지 폴트 서술은 추론이 주석에 사실처럼 굳은 것이었다. 주석을
  로그가 실제로 말하는 내용으로 교정했다.
- **그래서 headless A/B를 먼저 돌리지 않는다.** 크래시가 확률적이라 1회 재현으로는 정보량이 낮고,
  headless는 제출 경로뿐 아니라 ImGui/ESP 드로우 전체를 동시에 끄기 때문에 어느 쪽이 원인인지 구분도
  못 한다. 실측 데이터를 먼저 확보한다.
- **시스템 DRED를 Cyberpunk2077.exe 범위로 강제 활성화.** `d3dconfig.exe`가 이미
  `C:\Windows\System32`에 있고 관리자 권한 없이 설정이 적용됐다.
  - `d3dconfig apps --add "G:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe"`
    로 적용 범위를 게임 하나로 한정했다 (다른 D3D12 앱에 breadcrumb 오버헤드를 주지 않기 위해).
  - `d3dconfig dred --force-on-all` → `auto-breadcrumbs`, `breadcrumb-contexts`, `page-faults`,
    `watson-dumps`가 모두 `force-on`. **설정은 디바이스 생성 시점에 읽히므로 게임을 재시작해야 적용된다.**
  - 조사가 끝나면 `d3dconfig dred --force-sys-controlled-all`과 `d3dconfig apps --clear`로 되돌릴 것.
- **device removal 워치독 추가 (`overlay.cpp`).** 이번 크래시는 게임 핸들러가 프로세스를 먼저 죽여서
  Present 경로의 펜스 검사(`AcquireAllocator`)까지 도달하지 못했다. 디바이스를 얻은 직후 전용 스레드를
  띄워 8ms 주기로 `GetDeviceRemovedReason`을 확인하고, 실패하면 즉시 기존 `LogDeviceRemovedData`로
  DRED breadcrumb/page fault VA를 덤프한다. 스레드는 자기 `AddRef` 사본을 들고 돌아 렌더 뮤텍스를 잡지
  않으며, `ReleaseOverlayResources`에서 `g_device.Reset()` 전에 stop 이벤트로 신호를 주고 join하므로
  안전 언로드 순서를 깨지 않는다.
- Release 빌드는 `/W4`에서 경고 없이 통과. 라이브 검증은 게임 재시작 후 재주입해서 (1) 시작 로그에
  `device watchdog started`가 찍히는지, (2) 다음 크래시에서 `device watchdog observed removal` +
  `DRED breadcrumb[...]`/`DRED page fault VA=` 줄이 남는지 확인하는 것으로 한다.

## 2026-08-28 - DRED 실측: 메뉴 오픈 직후 device hung, ImGui 프레임 링 깊이가 원인

- **워치독이 데이터를 잡았다.** 08:27:16.090 `device watchdog observed removal`,
  `hr=0x887A0006`(DXGI_ERROR_DEVICE_HUNG), `DRED page fault VA=0x10C5863000`. 게임 자체 핸들러보다
  먼저 덤프하는 데 성공했다.
- **메뉴 오픈과의 상관은 3대 3이다.** 지금까지 GPU 사망 3건 모두 `overlay visibility toggled: visible=1`
  직후다: 00:20:14.622 -> 00:20:15.606, 07:52:52.357 -> 07:52:55.77, 08:27:15.014 -> 08:27:16.090.
  반대로 headless 구간(00:37~05:06, 64회)에는 device removal이 한 건도 없다. 오버레이 제출 경로가
  범인이라는 점은 이 시점에서 확정으로 본다. 다만 메뉴를 열고도 살아남은 토글이 여럿 있으므로
  (08:26:09, 08:26:34 포함) 확률적 레이스다.
- **원인 확정: ImGui DX12 백엔드의 프레임 링 깊이가 우리 in-flight 상한과 어긋나 있었다.**
  `vendor/imgui/backends/imgui_impl_dx12.cpp`를 읽어 확인했다.
  - 245행: `fr = &bd->pFrameResources[bd->frameIndex % bd->numFramesInFlight]`, 244행에서 frameIndex는
    `RenderDrawData` 호출마다(= 오버레이 제출마다) 1씩 증가한다.
  - 248~251행: 드로우 데이터가 커지면 그 슬롯의 정점 버퍼를 `SafeRelease`로 **즉시 해제**하고 더 큰 것을
    만든다. 펜스 대기가 없다. 리사이즈가 없어도 매 프레임 CPU가 그 버퍼에 memcpy한다.
  - 우리는 `NumFramesInFlight = g_bufferCount`, 즉 2를 넘겼는데, 같은 파일의 얼로케이터 풀은 DLSS Frame
    Generation 때문에 최대 `kMaximumAllocatorCount`(32)까지 커진다. 제출은 32프레임까지 in-flight가 될 수
    있는데 정점 버퍼 링은 2였다. GPU가 아직 읽는 버퍼를 해제하거나 덮어쓰는 창이 상시 열려 있었다.
  - 메뉴를 열면 드로우 데이터가 거의 0에서 한 번에 커져 248행의 해제 경로를 정확히 밟는다. 3건 모두
    메뉴 오픈 직후인 이유가 이것이고, page fault + DEVICE_HUNG이라는 증상과도 맞는다.
  - 수정: `initInfo.NumFramesInFlight`를 `kMaximumAllocatorCount`로 맞췄다. 링 깊이가 in-flight 상한과
    같으면 슬롯 i가 재사용될 때 그 사이에 32번의 제출이 있었다는 뜻이고, 얼로케이터 풀이 그만큼 재활용됐다는
    것은 해당 펜스가 완료됐다는 뜻이다. 프레임당 버퍼가 작아 메모리 비용은 무시할 만하다.
- **DRED breadcrumb은 여전히 비어 있었다.** page fault VA는 나왔는데 breadcrumb은 한 줄도 안 나왔다.
  `GetAutoBreadcrumbsOutput1`의 HRESULT와 노드 0개인 경우를 구분해 로그로 남기도록 고쳤다.
- **page fault 할당 노드 로깅 추가.** VA만으로는 누구 메모리인지 모른다.
  `pHeadExistingAllocationNode`와 `pHeadRecentFreedAllocationNode`를 각각 최대 32개까지 순회해
  오브젝트 이름과 `D3D12_DRED_ALLOCATION_TYPE`을 찍는다. 다음 크래시가 나면 폴트가 난 할당이 방금 해제된
  것인지, 그리고 그게 리소스인지 다른 오브젝트인지 이름으로 드러난다. 위 가설이 맞다면 recent freed 쪽에
  resource 타입 항목이 나와야 한다.
- Release 빌드는 `/W4`에서 경고 없이 통과. 다음 세션에서 게임 재시작 후 재주입해 메뉴를 반복적으로
  열고 닫으며 device removal이 사라졌는지 확인할 것.

## 2026-08-28 - ESP 커스텀 드로잉·에임봇 전멸의 원인: 투영 초기화가 실패를 영구 래치했다

- **증상**: 네이티브 하이라이트만 작동하고 바운딩 박스/스켈레톤/체력바가 전부 사라졌으며, 클래식·사일런트
  에임 양쪽 모두 반응이 없었다. UI 재배선 직후에 드러나서 UI 회귀로 보였지만 UI와 무관했다.
- **로그가 원인을 그대로 찍고 있었다.** `build/bin/Release/cp2077_trainer.log`:
  - `[08:36:54.576] projection unavailable: gameICameraSystem was not found`
  - 이후 세션 끝까지 모든 `ESP diagnostics`가 `projected=0 front=0 drawn=0 camera=0`. 엔티티는 정상
    (`snapshots=83`, 카테고리 분류·체력 모두 유효)이었고, 실패 지점은 오직 월드→스크린 투영이었다.
  - 같은 세션에서 `silent aim armed` / `memory aim active`가 한 줄도 없다. 에임봇도 후보 선별에
    `Projection::WorldToScreen`을 쓰기 때문에 같은 원인으로 함께 죽었다.
  - 네이티브 하이라이트만 살아남은 이유는 그 경로만 투영을 안 쓰기 때문이다. 사용자가 관측한 증상 조합이
    그대로 설명된다.
- **근본 원인**: `src/game/projection.cpp`의 `Initialize()`가 `g_state.attempted` 한 번으로 래치되는
  일회성 초기화였다. 실패하면 그 프로세스 수명 내내 투영이 죽은 채로 남는다. `GetCameraPosition`의
  `CameraPositionSource::Unavailable`도 같은 방식으로 영구 래치였다.
- **왜 지금 터졌나**: `tools/scripts/auto_inject_cp2077.cmd`(= `inject.py --auto`)로 게임 시작과 동시에
  주입하게 되면서, 첫 Present가 항상 메인 메뉴/로딩 시점에 잡힌다. 그때는 `gameICameraSystem`이 아직
  없다. 예전처럼 인게임에 들어간 뒤 수동 주입할 때는 첫 시도가 곧바로 성공해서 래치가 문제가 안 됐다.
  08:26 세션(수동 주입 추정)은 `projection initialized` 후 `projected=57`로 정상 동작했고, 08:36 세션
  (자동 주입)만 죽었다 — 로그에서 두 세션이 대조된다.
- **수정** (`src/game/projection.cpp`):
  - `Initialize()`를 `ResolveStatics()` + `ResolveCameraSystem()` + `EnsureCameraSystem()`으로 쪼갰다.
    주소 라이브러리 조회 결과만 한 번 캐시하고, 카메라 시스템 포인터는 250 ms 간격으로 다시 해석한다.
    실패는 더 이상 래치되지 않고, 세션 전환으로 포인터가 갈려도 따라간다.
  - 포인터가 바뀌면 `cameraPositionSource`와 first-projection 로그 플래그를 리셋한다.
  - `CameraPositionSource::Unavailable`은 2초마다 재시도한다. 한 번 고른 소스가 나중에 실패하면
    `Unknown`으로 되돌려 다시 고르게 한다.
  - 로그 스팸 방지: `loggedUnavailable` / `loggedCameraSource`로 상태가 바뀔 때만 찍는다.
  - 카메라 시스템 포인터가 이제 런타임에 바뀌므로 `std::atomic`으로 바꾸고, 사용하는 쪽이 한 번
    스냅샷해서 `ProjectRaw` / `ReadCameraPositionFrom*` / `IsCameraPositionPlausible`에 인자로 넘기도록
    했다. 다른 스레드가 그 사이에 널로 되돌린 포인터를 역참조하는 창을 없앤다.
  - 엔진 포인터 체인 역참조는 주입 직후 폴트 가능성이 있어 `ResolveCameraSystem`을 SEH로 감쌌다.
- **빌드**: `build-next`에서 Release 빌드 통과(경고 없음). `build/`는 게임이 DLL을 물고 있어 링크가
  막혔다 — 아래 항목 참고.

### 같이 발견한 별개 버그: 안전 언로드가 무한 루프에 빠진다

- `inject.py --unload`가 성공하지 않고 다음 3줄을 1초 주기로 무한 반복한다:
  `hook shutdown started` -> `no-recoil cleanup timed out: targetId=0x99F261` ->
  `hook shutdown aborted: main-tick feature cleanup did not acknowledge safely` ->
  `safe unload is waiting for hook cleanup; retrying in 1000 ms`.
- `src/dllmain.cpp`의 `while (!Hooks::Shutdown())` 루프에 포기 조건이 없어서, `PrepareForShutdown`이
  ack를 못 받는 상태가 되면 게임을 종료하는 것 외에 빠져나갈 방법이 없다.
- 메인 틱 자체는 돌고 있었다(같은 구간에 `visibility tick`이 계속 찍힘). 따라서 의심 지점은
  `PlayerModifiers::OnGameMainTick`의 정리 경로 — `ResolveRuntimeOnMainTick()`이 계속 실패해
  `g_cleanupAcknowledged`가 영영 안 서는 쪽이다. 아직 측정으로 확인하지 않은 가설이다.
- 아직 수정하지 않았다. 재현이 쉬우니 다음 작업 기점으로 잡을 것.

## 2026-08-28 - 중립에서 적으로 바뀐 NPC를 잡지 못하던 분류 문제

### 원인

- 기존 분류는 퍼펫에 캐시된 `isPolice` / `isCivilian` / `isGanger` 세 bool만 읽었다. 이 값들은 리액션
  프리셋 아키타입에서 유래하며 스폰 시점에 고정된다. "이 NPC가 무엇인가"이지 "지금 V를 어떻게
  대하는가"가 아니다.
- 스냅샷마다 다시 읽고 있었으므로 갱신 주기 문제는 아니었다. 값 자체가 정적이었다.
- 결과: 적대화된 시민은 계속 Civilian으로 남아 에임봇이 무시했고, 세 아키타입 어디에도 안 맞는
  경비·코퍼·퀘스트 NPC·드론은 상태와 무관하게 영구히 Other였다.

### 구현

- `NpcCategory`(스폰 아키타입)와 직교하는 `Hostility`(Unknown/Friendly/Neutral/Hostile) 축을 추가했다.
  스냅샷에 필드를 넣고 ESP와 에임봇이 둘 다 참조한다.
- 값은 게임 메인 틱에서만 읽는다. 체력 갱신과 동일하게 틱당 8개 라운드로빈이며,
  `ProcessAttitudeOnMainTick`이 플레이어의 attitude agent를 틱당 한 번 구한 뒤 NPC마다
  `GetAttitudeTowards`를 호출한다. Present 스레드는 캐시된 값만 복사한다.
- `EAIAttitude`는 `AIA_Friendly=0, AIA_Neutral=1, AIA_Hostile=2` (RED4ext.SDK 생성 헤더로 확인).
- ESP: 적대 상태면 아키타입과 무관하게 빨간색 `HOSTILE` 라벨을 쓰고 enemy 토글을 따른다. 경찰은
  적대 상태여도 자기 토글을 유지한다. 네이티브 하이라이트 게이팅도 같은 규칙을 쓴다.
- 에임봇 `IsEligible`: 경찰은 police 토글, 그 외에는 적대 상태이거나 갱 아키타입이면 enemy 토글.
  중립 갱단원을 미리 잡던 기존 동작은 그대로 남는다.

### 실측으로 잡은 함정: 리플렉션 함수를 베이스 클래스에서 찾으면 오버라이드가 실행되지 않는다

- `gameObject` CClass에서 찾은 `GetAttitudeAgent`를 플레이어 인스턴스로 invoke하면 호출 자체는
  성공하는데(`agentCalled=1`) 반환 핸들이 항상 null이었다. 스크립트 VM은 넘겨준 함수 객체를 그대로
  실행하므로 파생 클래스의 오버라이드가 아니라 베이스 선언이 돌아간다.
- 인스턴스의 실제 native type에서 조회하도록 바꾸니 `PlayerPuppet`과 `NPCPuppet` 모두
  `function=0x...BE4D7F40`(ScriptedPuppet 오버라이드)로 해석되고 agent가 정상 반환됐다.
  타입별 8칸 direct-mapped 캐시를 둬서 NPC마다 재조회하지 않는다.
- `GetAttitudeTowards`는 `gameAttitudeAgent`의 네이티브 함수(flags=0x1, params=1)라 이 문제가 없다.
- 교훈: 이 코드베이스에서 리플렉션 메서드를 캐시할 때는 선언 클래스가 아니라 호출 대상 인스턴스의
  타입에서 찾을 것. 안 그러면 "호출은 성공하는데 값이 비어 있는" 형태로 조용히 실패한다.

### 검증 상태

- 게임 2.31 / PID 14136 라이브: `attitude resolver ... resolved=1`,
  `attitude path: work=8 playerCalled=1 agentCalled=1 playerAgent=0x...B9D23A10 faulted=0`.
- 33개 퍼펫 추적 상황에서 `attitude[hostile=0 unknown=0 valid=8055 invalid=0]` — 전원 태도 해석 성공,
  당시 적대 대상이 없어 hostile은 0. 실제 중립→적대 전환 시 hostile이 올라가는지는 사용자 인게임
  확인 대기 중.
- `build/bin/Release`와 `build-next` 모두 Release 빌드 통과.
- 부수 정리: `silent_aim.cpp`의 로컬 `ClassNameHash`를 지우고 `Game::Rtti::ClassNameHash`로 통일했다.
  RTTI 열거용으로 `ParentClass` / `ClassNameHash` / `FunctionCount` / `FunctionAt`를 공개 API에 추가했다.

### 후속: 첫 구현이 게임을 프리즈시켰다 — 스크립트 함수를 메인 틱에서 돌리면 안 된다

- 위 "인스턴스 타입에서 조회" 수정으로 호출이 실제로 성사된 직후, 게임이 약 90초 만에 응답 없음
  상태로 멈췄다. `Get-Process ... Responding=False`, 트레이너 로그도 그 시점에 완전히 끊겼고
  `inject.py --unload`가 타임아웃했다. 프로세스를 강제 종료하는 것 외에 복구 방법이 없었다.
- 직전까지는 같은 코드가 안 멈췄는데, 그때는 베이스 클래스 함수라 agent가 null로 돌아와 조기 반환하느라
  `GetAttitudeTowards`까지 간 적이 없었다. 즉 멈춤은 "새로 실제 실행되기 시작한 호출"에서 왔다.
- 결정적 차이: `GetAttitudeAgent`는 **스크립트 함수**(flags=0xA600, native 아님)다. 체력 경로가 메인
  틱에서 돌리던 stat-pool 함수들은 전부 네이티브였다. 메인 틱 detour 안에서 스크립트 바이트코드를
  초당 1000회 규모로 실행한 것이 원인으로 판단된다.
- 수정:
  - agent를 스크립트 호출로 얻지 않는다. attitude agent는 그냥 엔티티 컴포넌트이므로 기존
    `ForEachComponent` + `IsClassOrDerived("gameAttitudeAgent")`로 찾는다. 순수 메모리 읽기다.
  - 남은 리플렉션 호출은 네이티브인 `GetAttitudeTowards` 하나뿐이다.
  - 스로틀링: 퍼펫당 250 ms 간격, 틱당 최대 4개, 사망 대상은 건너뛴다. 최악 케이스가 틱당 4회로,
    이미 검증된 체력 경로(틱당 24회)보다 가볍다.
  - 플레이어 agent 핸들은 500 ms 캐시한다. 패스마다 `ConstructHandle`/`ReleaseHandle`로 참조 카운트를
    흔들지 않기 위함이며, `Shutdown()`에서 해제한다.
- **규칙으로 남길 것**: 메인 틱에서 리플렉션 호출을 추가할 때는 먼저 `InspectFunction`의 flags bit0으로
  네이티브 여부를 확인한다. 스크립트 함수는 이 경로에서 호출하지 않는다.
- 재발 방지 차원에서 `tools/scripts/inject.py --auto` 감시 프로세스를 중단했다. 문제 있는 DLL이
  새 게임 프로세스에 자동으로 다시 들어가는 것을 막기 위함이며, 다시 켜려면 수동으로 재실행할 것.
- `build/bin/Release`와 `build-next` 모두 재빌드 통과. 수정 후 인게임 검증은 아직 못 했다.
- **인게임 검증 완료 (PID 8000, 09:4x)**: 살아 있는 퍼펫 20명 전원 태도 해석 성공, 실패 카운터는 18에서
  멈춘 뒤 증가 없음. Unknown 10개는 사망 대상 수와 정확히 일치했다(사망은 의도적으로 건너뛴다).
  적대 전환 후 `categories[civilian=2 ...] attitude[hostile=2 ...]`가 안정적으로 유지됐다 — 스폰
  아키타입이 civilian인 NPC 2명이 런타임 적대로 잡힌 것으로, 원래 보고된 실패 케이스 그대로다.
  프리즈 없이 게임 응답 정상 유지.

## 2026-08-28 - 10:12 크래시 / 10:16 프리즈: 원인은 게임이 올라간 USB 외장 SSD의 I/O 행

### 증상

- 10:12:23 게임이 하드 크래시. 트레이너 로그는 오류·DRED·device removed 없이 그냥 끊겼다.
- 10:14:03 재실행 → 10:16:32 인젝션 → 약 11초 뒤 프리즈. 마지막 트레이너 로그는 10:16:43.253.

### 실측

프리즈 상태의 프로세스(PID 9084)가 살아 있어서 미니덤프를 7분 간격으로 두 번 떴다
(`E:\cp2077_freeze_9084.dmp`, `..._b.dmp`). 심볼 없이도 모듈 귀속이 가능하도록
`tools/scripts/dumpwalk.py`를 새로 작성했다 (미니덤프의 모듈/스레드/메모리 스트림을 파싱하고
스택을 스캔해 모듈 내부를 가리키는 qword를 유사 콜스택으로 뽑는다).

- 게임 메인 틱 스레드(tid 2840)는 두 덤프에서 **RSP와 프레임이 완전히 동일**했고 RIP만
  `RtlQueryPerformanceCounter` 안에서 움직였다. 코어 하나를 100% 태우며 스핀 중이다
  (6초에 CPU 6.03초). 안쪽 프레임 `Cyberpunk2077.exe+0x14AC7B` / `+0x14AF14`의 바이트를 직접
  읽어보니 `lock cmpxchg` + 역방향 점프, 그리고 함수 포인터 predicate를 호출하는 재시도 루프였다.
  즉 게임 자체의 스핀 대기 프리미티브다.
- 파일 읽기 스레드(tid 33240)는 두 덤프 모두 `ZwReadFile+0x14`에서 **스택이 완전히 동일**했고
  스레드 CPU 시간이 3.219초에서 전혀 증가하지 않았다. 7분 넘게 반환되지 않은 동기 읽기다.
- 그 스레드의 R10(=NtReadFile의 HANDLE 인자) 0x1784를 살아 있는 프로세스에서
  `DuplicateHandle` + `GetFinalPathNameByHandle`로 풀었더니:
  `G:\CYBERPUNK_ARK_PACK_MO2\mods\Rogue_Rework 2K\archive\pc\mod\roguedowngrade.archive`
- G: 는 **USB(UASP) 외장 케이스에 든 WD SN740** (Get-Disk 기준 BusType=USB, 디스크 4번)이며
  게임과 MO2가 모두 여기 있다. 시스템 이벤트 로그에 10:13:05 UASPStor 이벤트 129(장치 리셋) +
  disk 이벤트 153(I/O 재시도) 12건이 찍혔다. 이 리셋은 최근 이틀간 8회 반복됐다.
- 같은 파일을 다른 프로세스에서 통째로 읽어보면 490 MB를 1.85초(265 MB/s)에 정상 읽는다.
  파일 손상이 아니라 in-flight IRP가 유실된 것이다.

### 결론

메인 틱이 스트리밍 잡을 스핀 대기하는데 그 잡이 의존하는 동기 읽기가 커널에서 영영 완료되지 않아
전체가 멈춘다. 유저 모드 훅은 syscall 안쪽을 붙잡을 수 없으므로 **트레이너도 모드도 원인이 아니다**.
10:12 크래시는 덤프가 없어 단정할 수 없지만, 42초 뒤 같은 디스크의 컨트롤러 리셋이 기록됐고
게임 텔레메트리(`%LOCALAPPDATA%\CD Projekt Red\Cyberpunk 2077\CrashInfo.json`)는
`isOom=false`, patch 2.31, LittleChina 로 남았다. 같은 스토리지 원인일 가능성이 높다.

### 조치

- 근본 대책은 게임과 MO2를 내장 드라이브로 옮기는 것. 그때까지는 케이블/포트/케이스 교체,
  USB 선택적 절전 해제 정도가 완화책이다.
- 다음 하드 크래시 때 덤프를 남기려면 관리자 PowerShell에서 WER LocalDumps를 켤 것:
  `New-Item -Path 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Cyberpunk2077.exe' -Force`
  뒤에 `DumpFolder`(ExpandString), `DumpCount`(DWord), `DumpType=2`(DWord, full) 설정.
- 트레이너 쪽 미비점: 예외 핸들러가 없어서 트레이너 안에서 폴트가 나도 로그에 아무 흔적이 남지
  않는다. 이번엔 트레이너가 원인이 아니었지만, 폴트 주소와 소유 모듈을 남기는 vectored exception
  handler를 붙여두면 다음 사고 때 귀속이 즉시 끝난다. 아직 구현하지 않았다.

### 부수 기록

- 덤프에서 확인한 OnTick 훅 체인 순서: 게임 → RED4ext → cp2077_trainer → CET → 게임 본체.
  트레이너 detour는 자기 작업(entity tracker / player modifiers / visibility)을 먼저 끝내고
  원본을 호출하므로, 프리즈 시점에 트레이너 작업은 이미 끝난 상태였다.
- 트레이너 스레드는 3개뿐이며 전부 정상 대기 중이었다(언로드 이벤트 대기, 메시지 대기).

## 2026-08-28 - 성능 최적화 Phase 0: QPC 구간 계측 도입

### 배경: 코드 구조상 비용이 확실한 다섯 지점

측정 데이터 없이 최적화하지 않기로 했다(DRED page fault 건에서 추정이 로그 대조로 뒤집힌 전례).
아래는 코드 리뷰로만 확인한 후보이며, 이번 커밋은 이것을 재기 위한 계측만 넣는다.

1. **스냅샷 전체 패스가 프레임당 2회 돈다.** `Esp::DrawOverlay`(esp.cpp)와 `Aimbot::RunFrame`(aimbot.cpp)이
   각각 `GetPuppetSnapshots`를 부른다. 150 FPS면 초당 300회 전체 패스이고, 128칸 static 스냅샷 배열도
   두 벌(각 ~220 KB) 존재한다.
2. **`GetPuppetSnapshots`가 배타 락을 잡은 채 무거운 일을 한다**(entity_tracker.cpp). 락 안에서 퍼펫마다
   `TrySnapshot`이 돌고, 그 안에 `ClassifyPuppet`(상속 체인 24단 x 7해시), `ClassifyNpc`(RTTI property 읽기),
   `IsCorpseDead`(컴포넌트 최대 512개 순회 x 체인 워크)가 매 호출 실행된다. NPC 카테고리는 스폰 시
   고정인데도 매번 재계산한다. 같은 락을 메인 틱의 health/attitude/highlight 경로가 기다린다.
3. **포즈 슬롯 읽기가 Present 스레드에서 reflected Invoke를 한다.** `ReadCurrentPoseSlots`가 33 ms마다
   퍼펫당 6회 `GetSlotTransform`을 `Rtti::Invoke`로 부르고, 매 호출이 `ForEachComponent` +
   `FindFunction`(클래스 함수 배열 선형 탐색)을 다시 한다. 성능 문제이자 "Present에서 VM 진입" 규칙과도
   긴장 관계다.
4. **시야 검사 처리량 부족.** `kRequestsPerTick = 1`, 갱신 주기 일괄 500 ms(visibility.cpp). 화면에 NPC가
   많으면 수요가 처리량을 초과한다 — drop 누계 95,939가 실측된 상태다.
5. **스냅샷 복사량 낭비.** `kMaxSkeletonSegments = 64`인데 MetaRig 제거 후 실제 세그먼트는 최대 5개다.
   퍼펫당 ~1.7 KB를 매 패스 memcpy하는데 ~85%가 빈 공간이다.

### 단계별 계획 (순서: 확실하고 안전한 것 -> 스레드 컨텍스트를 건드리는 것)

- **Phase 0** 계측 (이번 커밋).
- **Phase 1** 스냅샷 패스를 프레임당 1회로 통합. Overlay가 한 번 떠서 ESP와 에임봇에 같은 배열을 넘긴다.
- **Phase 2** `GetPuppetSnapshots` 경량화: 카테고리를 `TrackPuppet` 시점 캐시로, 정체성 검증을
  entityId + 캐시된 nativeType 포인터 비교로 축소, `IsCorpseDead` 250 ms 주기 제한, 락 구간을
  "짧은 락으로 식별 정보 복사 -> 락 밖에서 무거운 읽기 -> 짧은 락으로 visual write-back"으로 재구성,
  `kMaxSkeletonSegments` 64 -> 8.
- **Phase 3** 포즈 슬롯 읽기를 메인 틱으로 이전(health와 같은 라운드로빈 예산, 잠긴 타겟 우선).
  컴포넌트/함수 탐색 결과도 `TrackedPuppet`에 캐시. 유일하게 스레드 컨텍스트를 옮기는 변경이므로
  단독 커밋 + 단독 라이브 검증으로 격리한다.
- **Phase 4** 시야 검사 스케줄링: 갱신 주기 거리 적응형(근거리 250 ms / 원거리 1000 ms), 틱당 예산
  1 -> 2~4를 Phase 0 계측(캐스트 1회당 틱 소요)을 보고 단계적으로.
- **Phase 5** 메인 틱 미세 정리: `PublishHealth`/`PublishHostility`의 건당 O(256) 재탐색을 슬롯 인덱스
  전달로, 배치당 락 1회로 통합, `FindAttitudeAgent` 결과를 퍼펫별 캐시.

### Phase 0 구현

- `src/profiling.h` / `src/profiling.cpp` 추가 (`Diagnostics::Profile`). QPC 기반 RAII `Scope` +
  슬롯당 `count`/`total`/`max` relaxed 원자 누적기. 슬롯 하나당 비용은 QPC 2회 + 원자 연산 3회다.
- 로그는 **게임 메인 틱에서 5초마다 한 번** `LogCadence()`가 찍고 누적값을 리셋한다. Present에서 찍지
  않는 이유는 오버레이가 꺼져 있어도 계측이 남게 하기 위함이다. 두 줄로 나온다:
  - `profile present (5000ms): snapshot[...] snapLockWait[...] snapCount[...] poseSlots[...] esp[...] aimbot[...]`
  - `profile tick (5000ms): tickTotal[...] health[...] attitude[...] highlight[...] playerMods[...] visibility[...]`
  - 각 항목은 `n=표본수 avg=..us max=..us` 형식이고, `snapCount`만 시간이 아니라 패스당 스냅샷 개수다.
- 계측 지점:
  - `GetPuppetSnapshots` 전체 + 그 안의 `AcquireSRWLockExclusive` 대기 시간 분리 + 반환 개수.
  - `ReadCurrentPoseSlots` 1회(퍼펫당).
  - `Esp::DrawOverlay`, `Aimbot::RunFrame` 전체.
  - 메인 틱 detour: `TickTotal`(트레이너가 게임 틱에 얹는 총 지연, 원본 `OnTick` 호출은 제외)과 그 하위
    health / attitude / highlight / playerModifiers / visibility.
- **`__try` 제약**: MSVC는 객체 언와인딩이 필요한 함수에서 `__try`를 못 쓴다(C2712). 그래서 `TrySnapshot`,
  `ProcessHealthOnMainTick`, `ProcessAttitudeOnMainTick`, `ProcessNativeHighlightsOnMainTick`처럼 SEH를
  쓰는 함수 안에는 `Scope`를 넣지 않고, 그 **호출 지점**(`GetPuppetSnapshots`, `OnGameMainTick`)에서
  감쌌다. 새 계측을 추가할 때도 같은 규칙을 지킬 것.

### 검증 상태

- `build/bin/Release`, `build-next` 모두 Release 빌드 통과(경고 없음).
- 인게임 수치는 아직 없다. Phase 1 착수 전에 이 로그로 기준선을 먼저 뜬다.

## 2026-08-28 - 크래시 원인 규명 시스템: 트레이너 VEH(Vectored Exception Handler) 및 WER LocalDumps 도입

### 배경 및 목적

- 10:53:44 크래시(CityCenter 부두보이즈 전투 중) 분석 결과, 게임 엔진 내부 예외 필터가 동작하여 `CrashInfo.json`만 남기고 프로세스가 즉시 종료됨.
- 향후 발생하는 모든 돌발 크래시에서 트레이너 DLL, RED4ext 플러그인, CET Lua 브릿지, 게임 엔진 네이티브 코드 중 어느 모듈에서 폴트가 발생했는지 1초 만에 100% 식별하기 위해 실시간 예외 인터셉트 및 미니덤프 자동화 구축.

### 구현 내용

1. **트레이너 VEH (Vectored Exception Handler) 탑재 (`src/diagnostics.h`, `src/diagnostics.cpp`)**:
   - `AddVectoredExceptionHandler(1, VectoredExceptionHandler)`를 통해 OS 및 엔진 최상단에서 예외 인터셉트.
   - `IsFatalException(code)` 필터를 적용하여 C++ `throw`, CET 내부 Lua 예외(`0xE24C4A02`), RPC, Debugger print는 패스스루하고, 치명적 폴트(`0xC0000005`, `0xC0000409`, `0xC000001D`, `0xC00000FD`, `0xC0000374` 등)만 캐치.
   - **상세 진단 로그**:
     - 폴트 주소 및 소유 모듈명 (`GetModuleHandleExA` + `GetModuleFileNameA`), 모듈 상대 오프셋(`module+0xOFFSET`)
     - Access Violation 시 메모리 접근 유형(READ / WRITE / DEP) 및 타겟 메모리 주소
     - x64 전체 레지스터 (`RIP`, `RSP`, `RBP`, `RAX`~`RDX`, `RSI`, `RDI`, `R8`~`R15`, `EFLAGS`)
     - RSP 스택 역추적(Stack Walk)을 통한 상위 24개 모듈 리턴 주소 프레임 리스트
   - **자동 미니덤프 생성 (`MiniDumpWriteDump`)**:
     - `dbghelp.lib` 링크 및 크래시 발생 시 1회 `%LOCALAPPDATA%\cbpk\` 또는 빌드 폴더에 `cp2077_crash_<pid>_<timestamp>.dmp` 자동 저장.
2. **Windows WER LocalDumps 등록 스크립트 (`tools/scripts/enable_localdumps.ps1`, `tools/scripts/enable_localdumps.bat`)**:
   - 관리자 권한 1-클릭 실행으로 `HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Cyberpunk2077.exe` 레지스트리를 설정하여 하드 크래시 시 윈도우 차원의 풀 덤프 자동 저장.

### 검증

- `cmake --build build --config Release` 정상 빌드 완료.
- 실시간 라이브 주입 및 테스트 예외 인터셉트/콜스택 로그 출력 및 덤프 생성 정상 동작 검증 완료.

## 2026-08-28 - Phase 0 기준선 실측: 다섯 가설 중 둘이 뒤집혔다

### 표본

`build/bin/Release/cp2077_trainer.log`, PID 세션 tid=10204, 11:20:36 ~ 12:35:31 (75분, 프로파일 창 900개).
퍼펫 수(`snapCount`)로 구간을 나눠 가중평균했다. 세션의 93%는 퍼펫 2명 내외의 한산한 구간이고,
6%가 45명 내외의 혼잡 구간이었다.

| 슬롯 | 한산 (퍼펫 2, 531 fps) | 혼잡 (퍼펫 45, 179 fps) | 배율 |
|---|---|---|---|
| snapshot (1회) | 1.4 us | 49.2 us | 35.1x |
| snapLockWait | 0.0 us | 0.0 us | - |
| poseSlots (1회) | 6.9 us | 7.4 us | 1.07x |
| esp | 21.1 us | 150.9 us | 7.15x |
| aimbot | 7.3 us | 31.1 us | 4.26x |
| tickTotal | 82.9 us | 218.1 us | 2.63x |
| health | 26.1 us | 22.2 us | 0.85x |
| attitude | 17.7 us | 62.5 us | 3.53x |
| highlight | 0.7 us | 1.2 us | 1.71x |
| playerMods | 20.8 us | 71.6 us | 3.44x |
| visibility | 17.4 us | 60.3 us | 3.47x |

프레임/틱 대비 비중: 한산 구간에서 Present 28.4 us / 1.88 ms = 1.5%, 메인 틱 82.9 us / 1.89 ms = 4.4%.
혼잡 구간에서 Present 182 us / 5.6 ms = 3.3%, 메인 틱 218 us / 5.6 ms = 3.9%.

### 확인된 것

- **스냅샷 이중 패스는 정확히 사실이다.** `snapshot` 표본 수가 `esp` 표본 수의 **정확히 2.00배**
  (4,582,400 : 2,291,200). 다만 두 패스의 비용이 같지 않다. visual 캐시가 33 ms이므로 먼저 도는 ESP 패스가
  포즈 갱신을 떠안고, 뒤따르는 에임봇 패스는 캐시만 복사한다. 혼잡 구간에서 프레임당 스냅샷 총합이
  98.4 us, 그중 포즈 갱신이 41 us이므로 **싼 쪽 패스는 약 29 us**다. Phase 1의 실제 절감치가 이것이다
  (트레이너 Present 비용 182 us의 16%). 한산 구간에서는 0.7 us라 사실상 무의미하다.
- **포즈 슬롯 읽기가 스냅샷 비용의 최대 항목이다.** 세션 전체로 스냅샷 총 소요의 29%, 혼잡 구간
  프레임 기준으로는 98.4 us 중 41 us(42%)다. 호출당 7.4 us는 퍼펫 수와 무관하게 일정하다(1.07배) —
  즉 호출당 비용 자체가 비싸고, 총량은 순전히 호출 횟수에서 온다.

### 뒤집힌 것

- **락 경합은 없다.** `snapLockWait`가 표본 458만 건에서 가중평균 0.0 us, p95 0.0 us, 최댓값 93 us다.
  `g_puppetListLock` 배타 획득은 실질적으로 즉시 성사된다. Phase 2에서 가장 위험했던 "락 구간 3단
  재구성"(짧은 락 -> 락 밖 무거운 읽기 -> 짧은 락 write-back)은 **측정된 이득이 없으므로 하지 않는다.**
  Phase 2에서는 카테고리 캐시, `IsCorpseDead` 주기 제한, `kMaxSkeletonSegments` 축소만 남긴다.
- **시야 검사 병목은 이 세션에서 재현되지 않았다.** 75분 동안 `casts=5376`(초당 1.2회), `dropped=0`,
  큐는 계속 비어 있었다. 로그 앞쪽 다른 세션에 남아 있는 `dropped=99289`는 이 세션 것이 아니다.
  이번엔 visible-only 검사를 거의 켜지 않은 것으로 보인다. **Phase 4는 판단 근거가 없으므로 보류하고,**
  visible-only를 켠 전투 구간으로 별도 측정을 뜬 뒤에 착수한다.

### 새로 드러난 것 (계획에 없던 항목)

- **`health`는 퍼펫 수와 무관하게 고정 비용이다.** 퍼펫 2명일 때 26.1 us, 45명일 때 오히려 22.2 us다.
  틱당 24건 예산과 무관하게 `kMaxTrackedPuppets`(256) 슬롯을 매번 훑는 비용이 전부라는 뜻이다.
  세션의 93%를 차지하는 한산 구간에서 `tickTotal`의 31%를 혼자 먹는다. 계획에서 "효과는 작다"며
  마지막에 뒀던 **Phase 5가 실제로는 가장 흔한 상황의 최대 항목**이다.
- **혼잡 구간의 틱 비용 증가는 상당 부분 우리 루프 탓이 아니다.** NPC를 전혀 순회하지 않는
  `playerMods`가 3.44배 느려지는데, `attitude`(3.53배)와 `visibility`(3.47배)의 배율이 그와 거의
  같다. 즉 혼잡 구간에서는 리플렉션/VM 호출 경로 자체가 ~3.5배 느려진다. 이 경로들의 per-puppet
  루프를 줄여서 얻을 수 있는 몫은 표에 보이는 증가분보다 훨씬 작다.
- **`max` 열은 최적화 근거로 쓸 수 없다.** 프로파일 창 916개 **전부**에서 Present 스레드와 메인 틱
  스레드가 동시에 10 ms 넘는 스파이크를 기록했다(both=916, only-present=0, only-tick=0). 서로 다른
  두 스레드가 항상 같이 튄다는 것은 프로세스 전체가 멈춘다는 뜻이며, 트레이너 코드에 귀속되지 않는다
  (USB 외장 SSD I/O 행 건 참고). 앞으로 이 로그를 볼 때 신호는 avg이고 max는 게임 히칭 지표로만 읽는다.

### 갱신된 우선순위

1. **Phase 5를 먼저 한다** (한산 구간 최대 항목, 위험 낮음): `PublishHealth`/`PublishHostility`의
   건당 O(256) 재탐색 제거, 배치당 락 1회.
2. **Phase 1** 스냅샷 패스 통합 — 혼잡 구간 29 us/frame.
3. **Phase 2 (축소판)** 카테고리 캐시 + `IsCorpseDead` 주기 제한 + `kMaxSkeletonSegments` 64 -> 8.
   락 재구성은 제외.
4. **Phase 3** 포즈 슬롯 읽기 이전 — 혼잡 구간 41 us/frame으로 절대량은 가장 크지만 유일하게 스레드
   컨텍스트를 옮기는 변경이라 마지막에 단독으로.
5. **Phase 4** 보류. visible-only를 켠 측정 후 재판단.

## 2026-08-28 - 긴급 수정: VEH 동기 덤프 생성으로 인한 DLSS Frame Gen 데드락 해결

### 배경 및 증상

- 12:52:03경 렌더 스레드(TID `8136`)에서 엔티티 스트리밍 해제로 인한 정상적인 SEH 접근 위반(`0xC0000005`) 발생.
- 새로 추가되었던 VEH가 이를 하드 크래시로 오인하여 렌더 루프 도중 동기적으로 `MiniDumpWriteDump`를 호출함.
- MO2 `usvfs_x64.dll`을 거쳐 1.2GB 덤프를 디스크에 쓰는 1.3초 동안 전체 스레드가 강제 일시 정지되면서 NVIDIA Streamline (`sl.interposer.dll` / Frame Generation / Reflex)의 내부 프레임 펜스 동기화가 깨짐.
- 프로세스 재개 후, 메인 게임 스레드(TID `10204`)가 `sl.interposer.dll`의 펜스 대기에서 무한 대기(Deadlock)에 빠짐.

### 수정 내용

1. **VEH 내부 `MiniDumpWriteDump` 동기 호출 제거 (`src/diagnostics.cpp`)**:
   - 실시간 150 FPS 렌더 루프 중 전체 스레드를 일시 정지시키는 `MiniDumpWriteDump`를 VEH에서 완전 제거.
   - 치명적 하드 크래시 풀 덤프는 Windows WER `LocalDumps` (사후 크래시 덤프 엔진)에 전담.
   - VEH는 가벼운 1줄 로그(`[VEH] Exception ...`)만 남기고 즉시 `EXCEPTION_CONTINUE_SEARCH`를 반환하여 SEH(`__try / __except`)가 즉시 복구되도록 처리 (< 1us).
2. **엔티티 트래커 포인터 및 SEH 보호 강화 (`src/game/entity_tracker.cpp`)**:
   - `ReadTransform`: `transformComponent` 포인터가 유효 주소 범위(`0x10000` ~ `0x7FFFFFFFFFFF`) 내에 있는지 검증 추가.
   - `CaptureEntity`: `__try / __except` 보호 블록으로 감싸 스트리밍 중인 비정상 엔티티 등록 시에도 안전하게 스킵.

### 검증

- `E:/repos/cyberpunk2077-fix-veh` 워크트리에서 Release 빌드 완료 (`build/bin/Release/cp2077_trainer.dll`).
- SEH 접근 위반 발생 시에도 스레드 중단 없이 즉시 복구됨을 확인.

## 2026-08-28 - 세션 인계: VEH 수정 머지 + Phase 5 구현 완료 (라이브 검증 미완)

다음 작업을 깨끗한 세션에서 이어가기 위한 인계 기록이다. 위 "긴급 수정: VEH 동기 덤프" 항목은
`fix/veh-freeze-safe` 워크트리에서 작성된 것이고, 아래는 그것을 master에 머지하면서 확인·보완한
내용과 Phase 5 구현 상태다.

### 1. 머지 결과

- `fix/veh-freeze-safe` (커밋 `f468b9d`)를 master에 `--no-ff`로 머지했다. 분기점이 Phase 0 기준선
  커밋(`2162900`)이라 소스 충돌은 없었다.
- **머지하면서 고친 것 하나**: `CaptureEntity`를 `__try`로 감싼 변경에 락 누수 위험이 있었다.
  `AcquireSRWLockExclusive(&g_lastEntityLock)` 다음 줄이 `g_lastEntityId = entity->entityId`라
  거기서 폴트가 나면 `__except`로 빠지면서 `g_lastEntityLock`이 영영 잠긴 채로 남고, 이후
  `GetStats()`가 그 락에서 무한 대기한다(= ESP 진단 스레드 정지). 엔티티 ID를 락을 잡기 전에 지역
  변수로 읽어두고, 락 구간에서는 게임 메모리를 전혀 건드리지 않도록 바꿨다.
- VEH 로그 스팸 우려는 실측으로 기각했다. 전체 로그에서 VEH 인터셉트는 15건뿐이고, 그중 9건은
  11:18 언로드 재시도 루프에서 `KERNELBASE.dll+0xC187A`(RaiseException)로 초당 1건씩 찍힌 것이다.
  스냅샷 경로의 stale 폴트는 `staleRemoved=2` 수준으로 드물다. 1줄 로그를 남기는 비용은 문제없다.

### 2. 12:52 프리즈에 대해 기록으로 남길 사실 (해석과 분리)

측정된 사실만:

- 12:51:53경 `inject.py --unload`로 언로드 이벤트를 신호했다. 트레이너 로그에는
  `safe unload requested`도 `hook shutdown started`도 **찍히지 않았다**. 11:18의 정상 언로드
  때는 두 줄 다 찍혔다.
- 12:52:03.329 렌더 스레드(tid 8136)에서 `cp2077_trainer.dll+0xCAB9`가 `0xFFFFFFFFFFFFFFFF`를
  읽으려다 AV. 스택은 트레이너 → `nvwgf2umx.dll`(NVIDIA D3D12 드라이버) → 트레이너로 이어진다.
- 12:52:04.624 미니덤프 기록 완료. **파일 크기는 133,561,559 바이트(133 MB)다** — 위 항목의
  "1.2GB"는 실제 파일과 다르다. 결론(동기 덤프가 렌더 루프를 멈춘다)은 그대로 유효하다.
- 이후 로그가 끊겼고 PID 13196은 `Responding=False` 상태로 살아 있다.

아직 확정하지 못한 것:

- 언로드 요청과 AV의 인과. 워커가 로그를 한 줄도 남기지 않은 것은 "요청이 워커에 닿지 않았다"는
  뜻이고, 그러면 언로드가 D3D 자원을 해제해서 Present가 터졌다는 설명은 성립하지 않는다. 반대로
  이벤트 신호 후 정확히 10초(인젝터 타임아웃)만에 AV가 났다는 시간적 근접은 남는다. 다음에 언로드할
  때 이 두 줄이 찍히는지부터 확인할 것.

### 3. Phase 5 구현 (커밋 `da74847`) — 빌드 통과, 인게임 검증 미완

기준선에서 health가 퍼펫 2명일 때 26.1 us, 45명일 때 22.2 us로 **퍼펫 수와 무관한 고정 비용**이었기
때문에 계획의 마지막 단계였던 Phase 5를 1순위로 올려서 먼저 구현했다.

- **점유 비트맵**(`g_puppetOccupancy`, 256비트 = 32바이트). health/attitude/highlight의 라운드로빈이
  이제 이 비트맵을 보고 실제로 찬 슬롯만 만진다. 이전에는 매 틱 1 KB 넘는 엔트리 256개를 stride로
  훑었다. `TrackedPuppet::entity`가 여전히 단일 진실 원천이고, 비트맵은 같은 락 아래에서 갱신되며
  스캔이 착지한 엔트리는 항상 다시 검증한다. 세팅 지점은 `TrackPuppet` 하나, 클리어 지점은
  `GetPuppetSnapshots`의 stale 제거 하나뿐이다.
- **배치 발행**: `PublishHealthBatch` / `PublishHostilityBatch`가 배치당 배타 락 1회만 잡고, 수집
  시점의 슬롯 인덱스에 바로 쓴다. 값 하나당 전체 리스트를 훑던 것이 비교 1회로 줄었다. 슬롯이
  재활용됐을 수 있으므로 정체성 검증은 그대로 남아 있다.
- **attitude agent 캐시**: `TrackedPuppet::attitudeAgent`. `FindAttitudeAgent`는 엔티티의 모든
  컴포넌트를 훑는데 NPC마다 패스마다 돌고 있었다. 리플렉션 호출이 실패하면 캐시를 비워서 컴포넌트가
  교체된 경우 다음 패스에 자동 복구된다.
- **계측 추가**: `profile tickdetail` 줄이 새로 생긴다 —
  `healthCollect / healthInvoke / attitudeCollect / attitudeInvoke / highlightCollect`.
  기준선만으로는 health의 고정 비용이 슬롯 순회에서 온 것인지 리플렉션 호출에서 온 것인지 가를 수
  없었다. 이 다섯 값이 그 답을 준다.

**주의**: 이 커밋은 빌드만 통과했고 한 번도 게임에 들어가 본 적이 없다. 배포하려던 순간 위 프리즈가
났다.

### 4. 현재 빌드 상태

- `build-next/bin/Release/cp2077_trainer.dll` — 머지 + Phase 5 + 락 누수 수정이 전부 들어간 최신
  산출물. Release 빌드 통과.
- `build/bin/Release/cp2077_trainer.dll` — **구버전(11:18 빌드)이다.** 멈춘 게임 프로세스가 물고
  있어서 링크가 `LNK1104`로 막힌다. 프로세스를 정리한 뒤 다시 빌드해야 한다.

### 5. 다음 세션이 할 일 (순서대로)

1. PID 13196을 정리하고 게임 재시작. `cmake --build build --config Release`로 `build/`를 최신화.
2. Phase 5 DLL 주입 → 몇 분 플레이 → `profile tick`과 새 `profile tickdetail` 줄을 기준선과 대조.
   확인할 것: health의 고정 26 us가 줄었는가, 줄었다면 collect 쪽인가 invoke 쪽인가.
   attitude agent 캐시가 `attitudeInvoke`를 떨어뜨렸는가.
3. 결과를 기록하고 커밋한 뒤 **Phase 1**(스냅샷 패스 프레임당 1회 통합)로 진행.
4. 이후 **Phase 2 축소판**(카테고리 캐시 + `IsCorpseDead` 주기 제한 + `kMaxSkeletonSegments` 64→8,
   락 재구성은 제외), **Phase 3**(포즈 슬롯 읽기를 메인 틱으로), **Phase 4**는 보류 유지.
5. 마지막 단계에서 **로깅·프로파일링 마스터 토글**을 만든다. 개발 중이 아닐 때는 진단 로그와 QPC
   계측을 통째로 끄고 최고 성능으로 돌릴 수 있게 하는 것이 목표다.

### 6. 같이 남은 미해결 항목

- **Release 빌드가 PDB를 만들지 않는다.** 그래서 `cp2077_trainer.dll+0xCAB9`를 함수 이름으로 풀 수
  없었다. 크래시 도구를 붙여 놓은 마당에 심볼이 없으면 덤프의 가치가 절반이다. `/Zi` + `/DEBUG`를
  Release에 켜는 것을 검토할 것 (런타임 성능에는 영향 없음).
- **언로드 경로**. 11:18에는 `detour callbacks did not drain within 5000 ms` 후 재시도로 성공했고,
  12:52에는 워커가 로그를 한 줄도 남기지 못했다. 드레인 판정과 요청 전달 경로 둘 다 아직 믿을 수
  없는 상태다.
- **Phase 4 판단 보류**. 12:52 세션에서는 visible-only가 켜져 있었는데도(`visibility[on=1]`)
  75분간 캐스트 24,681회에 `dropped=0`이었다. 예전 로그의 drop 누계는 다른 세션 것이다.

## 2026-08-28 - Phase 5 첫 라이브 투입: 29초 만의 프리즈와 그 스택

### 준비 작업

- `build/`를 Phase 5 머지본(`c027856`)으로 재빌드. 이전 `build/bin/Release`는 11:18 구버전이었다.
- **Release PDB 생성을 켰다** (`CMakeLists.txt`). 인계 문서의 미해결 항목이었다. Release에
  `/Zi` + `/DEBUG`를 주되 `/OPT:REF`, `/OPT:ICF`를 함께 지정해 `/DEBUG`가 기본으로 끄는 최적화를
  되살렸다. 코드 생성은 그대로이고 `cp2077_trainer.pdb`(2.6 MB)가 나온다. 이 세션의 스택 덤프에서
  실제로 `HookOnTick+0xD5` 같은 이름이 풀린 것으로 효용을 확인했다.
- 프로세스 감시용 백그라운드 워치독을 붙였다 (20초 주기, 3연속 무응답이면 종료 코드 2로 빠져나와
  에이전트를 깨운다). 이번 프리즈를 60초 안에 잡아낸 것이 이 워치독이다.

### 무슨 일이 있었나

- 13:16:59 PID 27124(메인 메뉴)에 주입. 훅/오버레이 초기화 전부 정상, 첫 프레임 제출까지 로그 완결.
- 13:17:14 세이브 로드 완료. 엔티티 512개 등록, ESP 분류/투영/체력 읽기 모두 동작.
- 13:17:28.832 **메인 틱 스레드(tid 30216)에서 `0xC0000005`, `EXECUTE(DEP) target address 0x0`.**
  널 함수 포인터를 통한 간접 호출이다. 여기서 로그가 끊긴다.
- 13:17:35부터 프로세스 무응답. 워치독이 13:18:15에 알림.

### 스택 (디버거 없이 확보)

cdb/procdump이 이 머신에 없어서 `tools/scripts/threadstacks.py`를 새로 만들었다. 살아 있는(멈춰
있어도 되는) 프로세스의 각 스레드를 잠시 suspend하고 `GetThreadContext`로 RIP/RSP를 읽은 뒤, 스택
영역의 qword 중 실행 가능 메모리를 가리키는 값을 module+offset으로 환원한다. `dbghelp`로 PDB가 있는
모듈은 심볼까지 붙인다. 실제 스택 언와인딩이 아니라 후보 스캔이므로 오래된 잔여 값이 섞일 수 있다.

tid 30216의 프레임을 오래된 것부터:

```
usvfs_x64.dll+0x99230 -> Cyberpunk2077.exe 메인 루프
Cyberpunk2077.exe+0x9F3994 / +0x9F3A3E / +0x9F3283 / +0x9F32D0   (OnTick 주변)
RED4ext.dll+0x82020
cp2077_trainer.dll+0x10175  `anonymous namespace'::HookOnTick+0xD5
Cyberpunk2077.exe+0x291DF0
Cyberpunk2077.exe+0x9F33FA        <- 원본 OnTick 이어받는 지점 (훅 타겟은 +0x9F33C4)
MSVCP140.dll+0x17EAB -> 게임 코드 여러 프레임
Cyberpunk2077.exe+0x14A218
ntdll.dll+0x54E00                 <- 예외 디스패치 경계
Cyberpunk2077.exe+0x14A.../+0x14B.../+0x26C.../+0x245... (엔진 크래시 핸들러)
ntdll.dll+0x1A90D                 <- 현재 RIP, 여기서 대기 중
```

**읽어낼 수 있는 것**:

- 폴트는 우리 detour 본문이 아니라 **`g_originalOnTick` 호출 이후의 게임 코드**에서 났다. `HookOnTick`은
  EntityTracker/PlayerModifiers/Visibility 작업을 먼저 다 끝낸 다음 원본을 호출하므로, 예외 시점에
  우리 per-tick 코드는 이미 반환한 상태다.
- 우리 코드의 `__try` 블록 안이었다면 SEH가 잡아서 진행됐을 것이다. 엔진 크래시 핸들러까지 올라간
  것은 SEH 프레임이 없는 경로였다는 뜻이고, 이는 게임 코드였다는 위 판단과 일치한다.
- 트레이너 자체 스레드 둘(`MainThread`, `DeviceRemovalWatchdog`)은 프리즈 시점에 각각 언로드 이벤트
  대기와 sleep 상태로 정상이었다.
- 프로세스가 종료되지 않고 매달린 이유는 엔진 크래시 핸들러가 `ntdll`에서 대기 중이기 때문이다.
  그래서 `CrashInfo.json`도 WER 덤프도 생기지 않았다.

### 아직 확정하지 못한 것

트레이너가 원인인지 아닌지. 널 간접 호출은 게임 코드에서 났지만, 그 앞에 우리가 게임 상태를 바꾸는
동작을 여럿 하고 있었다. `config.ini`가 복원한 상태는 다음과 같았고 전부 세이브 로드 직후에 한꺼번에
발동했다:

- `native_highlight=1` — 13:17:15에 `braindance mode: 1`을 설정하고 하이라이트 이벤트 8건을 큐잉했다.
- `no_recoil=1` — 스탯 모디파이어 부착 시도(당시엔 `waiting-for-equipped-weapon`이라 미부착).
- `silent_aim=1` — 네이티브 크로스헤어 코어 훅 활성(`crosshairCore=1`).
- `visibility_check=1`, ESP 전체 on.

반대로 무죄 정황도 있다. **같은 기능 조합으로 돌린 직전 세션은 75분간 멀쩡했다.** 이번에 바뀐 변수는
Phase 5, VEH 머지, entity_tracker의 SEH 보강 셋뿐이다. 또 progress.md 앞쪽의 10:53:44 크래시는 Phase 5
이전에 이미 같은 형태(엔진 예외 필터가 삼키는 하드 폴트)로 발생했었다.

표본이 1건이라 귀속은 보류한다. 다음 세션에서 상태를 바꾸는 기능(native_highlight / no_recoil /
silent_aim)을 끄고 읽기 전용 경로만 켠 채 재현을 시도하는 것으로 갈린다. Phase 5가 손댄 health /
attitude 측정치는 그 구성에서도 그대로 나온다.

### 프리즈 직전까지 얻은 Phase 5 수치 (29초, 표본 부족)

메인 메뉴(퍼펫 0)와 세이브 로드 직후(퍼펫 27) 구간뿐이라 기준선의 "한산(2)/혼잡(45)"과 직접 비교할
수는 없다. 그래도 방향은 보인다.

| 슬롯 | 기준선 한산(퍼펫 2) | 기준선 혼잡(퍼펫 45) | 이번 퍼펫 0 | 이번 퍼펫 27 |
|---|---|---|---|---|
| health | 26.1 us | 22.2 us | 0.4 us | 7.4 ~ 13.6 us |
| attitude | 17.7 us | 62.5 us | 17.1 us | 29.7 ~ 40.1 us |
| tickTotal | 82.9 us | 218.1 us | 63.1 us | 90.2 ~ 167.8 us |

- **health의 퍼펫 수 무관 고정 비용은 사라졌다.** 퍼펫 0에서 0.4 us다. 점유 비트맵이 의도대로
  동작한다는 뜻이다. 새 `tickdetail` 줄이 그 근거를 나눠 보여준다: `healthCollect`는 퍼펫 27에서도
  0.1 us이고 `healthInvoke`가 6.8~13.0 us다. 즉 **기준선의 26 us는 전부 슬롯 순회였고, 실제 리플렉션
  호출은 원래도 싸다.**
- `attitudeCollect`도 0.2~0.3 us, `attitudeInvoke`는 호출당 3.3~3.6 us로 안정적이다. 다만 `attitude`
  전체(29.7~40.1 us)와 `attitudeInvoke`(n=138~160, 3.6 us) 사이의 차이가 크다. attitude 슬롯에는
  플레이어 에이전트 재해석과 `ResolveAttitudeOnMainTick`이 포함되므로 그쪽을 따로 봐야 한다.
- 표본 시간이 29초뿐이고 세이브 로드 직후의 스트리밍 구간이 섞여 있으므로 **이 표는 확정치가 아니다.**
  재현 세션에서 다시 뜬다.

## 2026-08-28 - 재현 2회차: VEH를 꺼도 같은 자리에서 같은 방식으로 멈춘다

### VEH 감사 결과 (사용자 지적에 따라 착수)

사용자가 "75분 무사고 세션과 25초 만에 뻗은 세션의 결정적 차이는 VEH이고, 그 VEH는 약한 모델이 짠
것이라 믿을 수 없다"고 지적해 코드를 먼저 감사했다. 실제 결함이 넷 나왔다.

1. **`AddVectoredExceptionHandler(1, ...)`** — 우선순위 1은 체인의 맨 앞이다. 게임/RED4ext/CET의
   핸들러보다 먼저 끼어든다. 관측만 하고 통과시키는 핸들러가 가질 이유가 없는 위치다.
2. **재진입 가드 없음.** 핸들러 안에서 폴트가 나면 (아래 3, 4 때문에 충분히 가능했다) 즉시 재진입해
   무한 재귀한다. 핸들러가 쓰는 스택은 `modName`/`fullPath`/`message`/`line` 합쳐 4.8 KB가 넘는데
   `EXCEPTION_STACK_OVERFLOW`까지 fatal로 잡고 있었다. 스택 오버플로 시 확실히 죽는 구조다.
3. **예외 문맥에서 로더 락을 잡는다.** `GetModuleHandleExA` / `GetModuleFileNameA`가 그렇다. 폴트난
   스레드가 이미 로더 락을 들고 있거나 다른 스레드가 들고 있으면 데드락이다. 이 게임은 애셋을
   스트리밍하고 RED4ext/CET가 플러그인 DLL을 로드한다.
4. **복구 가능한 예외를 fatal로 분류했다.** `STATUS_GUARD_PAGE_VIOLATION`은 윈도우가 스택을 늘리는
   방식이고, `EXCEPTION_IN_PAGE_ERROR`는 메모리 맵 파일 페이징이며, 이 게임은 엔티티 스트리밍 중
   `EXCEPTION_ACCESS_VIOLATION`을 정상적으로 내고 자체 SEH로 복구한다 (progress.md 12:52 항목).

여기에 더해 **로그가 향하는 `E:`가 기계식 SATA 하드디스크(WDC WD10EZEX, 7200rpm)라는 것**을 확인했다.
`WriteLine`은 매 줄마다 `FlushFileBuffers`를 호출한다. 즉 VEH 로그 한 줄이 예외 디스패치 도중 메인 틱
스레드에서 일어나는 물리 디스크 seek + 플래터 쓰기였다. 12:52에 프레임 생성 펜스를 깨뜨린 동기 미니덤프와
같은 부류이고 크기만 작다.

### 수정 내용

- 핸들러는 이제 **락 없는 링 버퍼(32칸)에 POD 6개만 적고 반환한다.** 포맷도, 모듈 조회도, 락도, 디스크
  I/O도 하지 않는다. `Diagnostics::DrainExceptionLog()`가 메인 틱의 5초 cadence에서 링을 비우며,
  모듈 조회와 파일 쓰기는 거기서 한다 (평범한 틱 문맥이라 안전하다).
- 우선순위 0(맨 뒤)으로 등록. 재진입 가드(`thread_local`) 추가. fatal 목록에서 guard page / in-page
  error / FLT 계열 / misalignment / array bounds 제거.
- **기본값을 off로 바꿨다.** `CBPK_VEH=1`일 때만 등록한다. 관측 장치가 프로세스 전체 예외 경로에
  상주하는 것 자체가 게임 동작을 바꾸는 변수이기 때문이다.

### 그런데 VEH를 끄고도 똑같이 멈췄다

- 13:36:16 PID 25756에 주입. `veh=disabled` 확인. `native_highlight=0`, `no_recoil=0`, `silent_aim=0`
  으로 상태 변경 기능을 끄고 ESP/시야 검사/체력/적대도만 켠 구성.
- 13:36:53.473 로그가 끊김. 주입 후 **37초**. 1회차는 29초였다.
- 13:37:12부터 무응답, 워치독이 13:37:52에 알림.

**두 번의 정지가 같은 것이라는 근거:**

- 멈춘 스레드의 스택 최상단이 두 번 모두 `Cyberpunk2077.exe+0x14AC7B` / `+0x14AF14`다. 1회차에서 이
  주소들은 `ntdll` 예외 디스패치 경계보다 위(더 최근)에 있었다. 엔진 크래시 핸들러다.
- 두 번 모두 세이브의 같은 지점이다. ESP 진단이 그것을 못박는다:
  `categories[civilian=11~12 enemy=6 police=2 other=7]`, `depthRange=[5.32,72.22]`가 양쪽에서 동일하다.
- 2회차에서는 CPU 시간이 계속 증가하고 있었다(1코어 100%). 데드락이 아니라 **크래시 핸들러가 나머지
  스레드 101개를 전부 suspend해 놓고 혼자 도는 상태**다. `ThreadState` 집계로 확인:
  `Suspended=101, Running=1`. 프로세스가 끝내 종료되지 않으니 `CrashInfo.json`도 WER 덤프도 안 생긴다.

### 그래서 VEH는 방아쇠가 아니다

VEH를 완전히 끈 빌드가 같은 자리에서 같은 방식으로 멈췄으므로 이번 정지의 원인은 VEH가 아니다. 위
수정은 그 자체로 옳으므로 되돌리지 않지만, 원인 후보에서는 뺀다.

남은 변수는 **Phase 5**와 **`CaptureEntity`의 SEH 보강** 둘이다. 그리고 아직 한 번도 확인하지 않은
가능성이 하나 더 있다: **그 세이브 지점이 트레이너 없이도 죽는가.** 두 번 다 같은 세이브 같은 위치라
이 대조군이 없으면 우리 코드에 귀속할 수 없다.

### 정지 직전 수치에서 눈에 띄는 것

- `health[valid=5141 invalid=13630]` — 체력 읽기 실패가 성공의 2.6배다. 유효하지 않은 엔티티를 자주
  건드리고 있다는 뜻이고, 재활용된 메모리를 짚었다면 SEH로는 걸러지지 않는다.
- 마지막 창에서 `healthInvoke`가 6.7 us -> 29.4 us, `attitude`가 43.9 -> 133.7 us, `tickTotal`이
  88.4 -> 218.0 us로 뛰고 max가 각각 86 ms / 120 ms다. 정지 직전에 이미 게임이 무너지고 있었다.
- 반면 Phase 5 자체의 효과는 이 세션에서도 확인된다: `healthCollect`는 퍼펫 26명에서도 0.1~0.2 us이고
  `playerMods`는 no-recoil을 꺼서 0.1 us다.

## 2026-08-28 - 원인 규명: Phase 5의 attitude 컴포넌트 포인터 캐시

### 바이섹트

세 번의 실측으로 좁혔다.

| 실행 | 구성 | 결과 |
|---|---|---|
| 대조군 (사용자) | 트레이너 미주입, 같은 세이브 | 3~5분 이상무 |
| 1회차 | Phase 5 + VEH on + 전체 기능 | 29초 만에 정지 |
| 2회차 | Phase 5 + VEH off + 상태변경 기능 off | 37초 만에 정지 |
| 3회차 | **Phase 5만 revert**, 나머지 2회차와 동일 | **7분 20초 무사고** |

3회차는 앞의 두 정지보다 훨씬 무거운 부하였다(퍼펫 65, 적 19, 전투, 시야 캐스트 11,559회). 구성 차이가
Phase 5 하나뿐이므로 원인은 Phase 5다. 세이브 지점도 VEH도 아니다.

### 원인

`ReadHostility`가 NPC의 `gameAttitudeAgent` 컴포넌트를 매 패스 새로 찾아 지역 변수로 쓰던 것을, Phase 5가
`TrackedPuppet::attitudeAgent`에 raw 포인터로 캐시해 틱을 넘겨 재사용하도록 바꿨다. 컴포넌트 목록을 매번
훑는 비용을 아끼려는 의도였다.

그 컴포넌트는 NPC의 소유물이라 NPC가 스트리밍 아웃되면 해제된다. 그런데 캐시된 포인터는 계속
리플렉션 VM 호출의 `this`로 전달됐다. **`__try`는 여기서 아무 소용이 없다** — 해제된 메모리가 다른
객체로 재활용되면 포인터는 멀쩡히 읽히므로 예외가 나지 않는다. 호출이 그냥 엉뚱한 객체에 착지해서
그것을 망가뜨린다. 1회차 VEH가 기록한 `EXECUTE(DEP) target address 0x0`(게임 코드에서의 널 간접 호출)이
그 결과이고, 월드가 스트리밍된 뒤 11~14초라는 지연도 이것으로 설명된다.

원 구현의 "호출이 실패하면 캐시를 비워서 자가 치유한다"는 설계는 **실패하는 경우만 덮는다.** 위험한
경우는 성공하는 경우다.

### 수정

캐시를 제거하고 Phase 5의 나머지는 유지했다. 이게 옳은 교환인 이유는 계측이 말해준다. 측정된 이득은
점유 비트맵에서 나왔고, attitude 컴포넌트 캐시가 노린 비용은 애초에 유의미한 적이 없었다
(`attitudeInvoke` 호출당 3.6 us). 위험 대비 이득이 Phase 5 안에서 가장 나빴다.

같은 게임 프로세스(PID 34128)에 안전 언로드 후 수정본을 재주입해 **10분 17초, 프로파일 창 123개,
퍼펫 최대 86명**까지 무사고를 확인했다. 인계 문서가 신뢰할 수 없다고 적어 둔 언로드 경로도 이번엔
로그가 완결되고 게임이 계속 응답했다.

## 2026-08-28 - Phase 5 실측 결과 (10분, 창 126개)

퍼펫 수로 구간을 나눠 표본 수 가중평균했다. 시간 단위는 us.

| 슬롯 | 퍼펫 0 (창 1) | 퍼펫 21 (창 20) | 퍼펫 41 (창 105) |
|---|---|---|---|
| tickTotal | 409.3 | 287.9 | 147.5 |
| health | 0.5 | 26.6 | 22.0 |
| healthCollect | 0.3 | 0.21 | 0.15 |
| healthInvoke | - | 24.2 | 21.4 |
| attitude | 185.5 | 79.6 | 40.7 |
| attitudeCollect | 0.2 | 0.47 | 0.58 |
| attitudeInvoke | - | 5.65 | 6.72 |
| visibility | 100.8 | 73.8 | 38.3 |
| snapshot (1회) | 0.7 | 31.3 | 31.8 |
| poseSlots (1회) | - | 8.38 | 8.41 |
| esp | 73.8 | 160.8 | 109.9 |
| aimbot | 57.3 | 27.9 | 24.1 |

### 확인된 것

- **점유 비트맵은 의도대로 동작한다.** `healthCollect`가 퍼펫 41명에서도 0.15 us다. 기준선에서 health
  전체를 먹던 256슬롯 순회가 사실상 사라졌다.
- **다만 이득은 리스트가 빈 구간에 집중된다.** 퍼펫 0에서 health 0.5 us(기준선은 퍼펫 2에서 26.1 us)인
  반면, 퍼펫 41에서는 22.0 us로 **기준선의 혼잡 구간 22.2 us와 사실상 같다.** 기준선 세션의 93%가
  한산 구간이었으므로 실사용 이득은 여전히 크지만, "health가 전 구간에서 싸졌다"고 말하면 틀린다.

### 새로 드러난 다음 목표 (Phase 5의 계측이 없었으면 안 보였다)

1. **`healthInvoke`가 이제 메인 틱 최대 항목이다.** 틱당 21~24 us이고 이것이 health 비용의 전부다.
   틱당 8건 예산이니 리플렉션 호출 1회가 약 2.7 us다. Phase 5는 이 부분을 건드리지 않았다.
2. **`attitude`에 설명되지 않는 38.7 us가 있다.** 퍼펫 41 기준 attitude 40.73 us 중 collect가 0.58,
   invoke 기여분이 6.72 x (24877/115734) = 1.44 us뿐이다. 남는 38.7 us는 `ResolveAttitudeOnMainTick()`과
   플레이어 에이전트 핸들 갱신이다. 이건 **`workCount == 0` 검사보다 먼저, 할 일이 없는 틱에도 매번
   호출된다.** 호출 위치만 옮겨도 상당 부분이 사라질 가능성이 높다. 위험이 낮으니 다음 순번으로.
3. **Phase 1(스냅샷 이중 패스)은 그대로 남아 있다.** `snapshot` 표본 수가 `esp`의 정확히 2.00배
   (231,468 : 115,734)로 재확인됐다. 퍼펫 41에서 패스당 31.8 us다.

### 갱신된 순서

1. attitude의 무조건 resolve 호출 위치 교정 (위 2번, 가장 싸고 안전하다).
2. Phase 1 스냅샷 패스 통합.
3. Phase 2 축소판 (카테고리 캐시, `IsCorpseDead` 주기 제한, `kMaxSkeletonSegments` 64 -> 8).
   **단, Phase 5의 교훈을 적용한다: 게임 객체 포인터를 틱 너머로 캐시하지 않는다.** 값은 캐시해도
   되지만 포인터는 안 된다. 캐시한다면 그 객체의 수명을 우리가 알 수 있어야 한다.
4. `healthInvoke` 자체 (호출당 2.7 us x 8/틱).
5. Phase 3 포즈 슬롯 이전, Phase 4는 계속 보류.

### 부수적으로 확인/수정한 것

- 로그가 향하는 `E:`가 기계식 HDD인데 `WriteLine`이 매 줄 `FlushFileBuffers`를 부른다. 아직 안 고쳤다.
  마스터 토글(계획 5단계)을 만들 때 같이 처리할 것.
- `tools/scripts/threadstacks.py`가 이번 조사에서 두 번 다 결정적이었다. 디버거 없이 멈춘 프로세스의
  스레드 상태를 보는 유일한 수단이다.
- 워치독 스크립트(20초 주기, 3연속 무응답이면 알림)가 두 정지를 각각 60초 안에 잡았다.

## 2026-08-28 - 로깅이 게임 스레드를 세우고 있었다: 줄당 20.8 ms

직전 항목에서 "로그가 향하는 E:가 기계식 HDD인데 `WriteLine`이 매 줄 `FlushFileBuffers`를 부른다.
아직 안 고쳤다"고 적어 둔 것을 처리했다. 사용자가 "로그를 C:(NVMe)의 사용자 local 경로로 옮기면
낫지 않겠나"라고 제안했고, 재 보니 방향은 맞았지만 디스크만 바꿔서는 절반도 못 건진다.

### 실측 (호출자 스레드가 `Log()` 한 번에 붙잡히는 시간, 300회 평균)

게임 실행 중에 측정했다. 벤치는 `Diagnostics::Log`와 예전 경로를 같은 조건에서 나란히 돌린다.

| 경로 | 평균 | 최악 |
|---|---|---|
| 예전: `WriteFile` + `FlushFileBuffers`, 로그가 E: (HDD) | 20816 us | 91443 us |
| 예전 코드 그대로, 로그만 C: (NVMe)로 옮긴 경우 | 1007 us | 4573 us |
| `OutputDebugStringA` 1회 (디버거 없음) | 2.6 us | 55 us |
| 지금: 링 큐 + writer 스레드 | 0.95 us | 3.8 us |
| 지금, `CBPK_LOG=0` | 0.02 us | 0.10 us |

**줄당 20.8 ms다.** 라이브 로그의 정상 구간이 초당 5~8줄, 버스트가 초당 30줄이었으니 평시에도
게임 스레드 하나가 초당 100~160 ms를 디스크 대기로 날리고 있었고, 버스트 1초는 그대로 0.6초짜리
멈춤이었다. 오버레이의 오류 경로(`invalid back-buffer index`, `overlay rendering disabled after
fence/device failure` 등)는 조건이 성립하면 매 프레임 찍히므로, 그중 하나만 켜져도 로깅만으로
프레임 타임이 20 ms 늘어난다. 지금까지 본 히칭 중 일부는 이것이었을 가능성이 높다.

디스크만 C:로 옮겼으면 20816 -> 1007 us. 20배지만 여전히 줄당 1 ms이고, 144 Hz에서는 한 프레임을
통째로 먹는다. **`FlushFileBuffers`를 줄마다 부르는 것 자체가 문제였고 디스크는 그 위에 얹힌 배수다.**
둘 다 고쳤다.

### 수정 내용 (`src/diagnostics.cpp`, `src/diagnostics.h`)

1. **비동기 writer.** 호출자는 스택에서 포맷해 락 없는 링 버퍼(Vyukov bounded MPMC, 512 슬롯 x
   1 KB)에 넣고 즉시 반환한다. 전용 writer 스레드가 큐를 비우면서 여러 줄을 하나의 `WriteFile`로
   합쳐 쓴다. 게임 스레드가 하는 일은 `vsnprintf` 하나, `memcpy` 하나, `SetEvent` 하나다.
2. **`FlushFileBuffers`를 줄 단위에서 뗐다.** writer 스레드가 1초 cadence로만 부르고, 그 밖에는
   명시적 `Diagnostics::Flush()`(초기화 직후, 미니덤프 전후, 종료 시)에서만 부른다. 프로세스가
   죽어도 `WriteFile`까지 끝난 내용은 파일 시스템 캐시에 남으므로 크래시 진단 가치는 유지된다.
   cadence flush가 막는 것은 그보다 위, 머신이 통째로 굳어 전원을 내리는 경우다.
3. **`OutputDebugStringA`를 기본 off로.** 디버거가 없어도 매번 SEH 예외(`DBG_PRINTEXCEPTION_C`)를
   일으키고, `CBPK_VEH=1`이면 우리 VEH까지 그 경로를 탄다. 줄당 2.6 us(최악 55 us)였다. 이제
   `CBPK_DBGOUT=1`일 때만 켜지고, 켜더라도 호출은 writer 스레드에서만 일어난다.
4. **로그/덤프 위치를 `%LOCALAPPDATA%\cp2077_trainer\`로.** `CBPK_LOG_DIR`로 덮어쓸 수 있고,
   디렉터리를 못 만들거나 파일을 못 열면 예전처럼 DLL 옆으로 되돌아간다. 빌드 산출물 디렉터리
   밖이라 클린 빌드에 쓸려나가지 않는 이점도 같이 얻는다. 133 MB짜리 미니덤프도 여기로 간다.
5. **`CBPK_LOG=0` 킬 스위치.** 계획 5단계의 마스터 토글 중 로그 쪽만 먼저 넣었다. QPC 계측
   토글은 아직이다.
6. **32 MB 넘으면 한 번 롤링.** 예전 로그는 9.7 MB까지 갔고 이제 시스템 드라이브에 쌓이므로,
   초기화 시점에 크기를 보고 `.<타임스탬프>.old`로 옮긴다.
7. 링이 꽉 차면 게임 스레드를 세우지 않고 버리며, 버린 줄 수를 writer가 로그에 남긴다.

### 검증

- 빌드: `cmake --build build-next --config Release` 통과, 경고 0.
  (`build/`는 게임이 DLL을 물고 있어 링크가 막혀서 `build-next/`에 빌드했다.)
- **락 없는 큐 스트레스 테스트**: 실제 TU를 링크해 8스레드 x 20,000줄 = 160,000줄을 동시에 밀어넣고,
  종료 후 파일을 다시 읽어 검증. 3회 모두 `기록된 줄 + 버린 줄 == 보낸 줄`이 정확히 일치했고
  찢어지거나 섞인 줄이 0이었다. 초당 260만 줄이라는 비현실적 부하에서 1~6%가 드롭됐는데, 실사용
  부하는 초당 30줄이라 드롭이 날 여지가 없다.
- 기본 경로가 `%LOCALAPPDATA%\cp2077_trainer\`로 잡히는 것, `CBPK_LOG_DIR` 덮어쓰기, `CBPK_LOG=0`,
  32 MB 롤링을 각각 실행해서 확인했다.
- 테스트 코드는 스크래치패드에 있고 저장소에 넣지 않았다.

### 아직 안 한 것

- **인게임 검증.** 측정 당시 게임(PID 34128)이 예전 DLL을 물고 있어 `build/`를 링크할 수 없었다.
  다음 세션에서 트레이너를 언로드(End 또는 `inject.py --unload`)한 뒤 `build/`를 다시 빌드해
  주입하고, `log sink:` 줄이 `%LOCALAPPDATA%` 경로와 `async=on`을 보고하는지 확인할 것.
- QPC 계측 마스터 토글(계획 5단계의 나머지 절반).

## 2026-08-28 - 진단 토글을 config.ini로 빼고, 새 로깅 경로를 인게임에서 확인

### 왜 환경 변수만으로는 안 됐나

이미 떠 있는 게임에 나중에 주입하는 것이 이 프로젝트의 기본 워크플로인데, 그 프로세스의 환경
변수를 밖에서 심을 방법이 없다. `inject.py`는 `CreateRemoteThread` + `LoadLibraryW`라 환경에는
손을 못 댄다. 그래서 `CBPK_VEH=1`을 켜려면 게임을 그 변수와 함께 재시작해야 했다. 최적화 마무리
단계에서 로깅·계측을 다 끄고 최고 프레임을 재려면 같은 문제가 반대 방향으로 다시 생긴다.

### 수정 내용

- 진단 토글을 `%LOCALAPPDATA%\cbpk\config.ini`의 `[diagnostics]` 섹션에서도 읽는다. 해석 순서는
  **환경 변수 > ini > 컴파일 기본값**이다. 환경 변수를 위에 둔 것은 특정 실행에서만 일회성으로
  덮어쓰는 용도를 남겨두기 위해서다. ini에 키가 없으면 기본값을 한 번 써 넣어 파일만 봐도 어떤
  스위치가 있는지 알 수 있게 한다.
  - `logging` / `CBPK_LOG` (기본 1)
  - `profiling` / `CBPK_PROFILE` (기본 1)
  - `veh` / `CBPK_VEH` (기본 0)
  - `debug_output` / `CBPK_DBGOUT` (기본 0)
  - `Config::Initialize`보다 먼저 필요하므로(로그 파일이 그 전에 열려야 한다) `diagnostics.cpp`가
    같은 ini를 직접 읽는다. Config 모듈과 파일만 공유하고 코드 의존은 없다.
- **계측 마스터 토글을 넣었다** (계획 5단계의 나머지 절반). `Profile::Scope`가 꺼져 있으면 QPC를
  아예 부르지 않는다. 생성자에서 `Enabled()`가 false면 `start_`가 0으로 남고 소멸자가 통째로
  빠진다. `Record`와 `LogCadence`도 같은 스위치를 본다.
- 초기화 시 `diagnostics toggles: logging=.. profiling=.. veh=.. dbgout=..` 한 줄을 남긴다. 나중에
  로그만 보고 그 세션이 어떤 조건이었는지 알 수 있어야 한다.

### 인게임 확인 (PID 26508, 2560x1440)

빌드 후 주입해서 확인한 것:

- 로그가 `C:\Users\admin\AppData\Local\cp2077_trainer\cp2077_trainer.log`에 생기고 `async=on`.
- `diagnostics toggles: logging=1 profiling=1 veh=1 dbgout=0`, VEH `active`.
- 훅 전부 활성화, 오버레이 초기화 완료, 첫 프레임 제출까지 정상. 게임 응답 정상.
- 큐 드롭 0줄, VEH 기록 0건.

한산한 씬(퍼펫 0)의 5초 창 수치. 게임플레이 표본이 아니므로 최적화 판단 근거로 쓰면 안 된다:

| 슬롯 | avg | max |
|---|---|---|
| tickTotal | 3.6 us | 17.6 us |
| esp | 0.8 us | 48.8 us |
| aimbot | 6.8 us | 95.7 us |
| snapshot (1회) | 0.4 us | 48.5 us |

- `snapshot` 표본 수가 `esp`의 정확히 2.00배로 다시 확인됐다 (4666 : 2333). **Phase 1의 스냅샷
  이중 패스는 그대로 남아 있다.**
- Present 레이트가 초당 약 509회였다.

### 같이 발견한 것 (안 고침)

`%LOCALAPPDATA%\cbpk\config.ini`가 **UTF-8 BOM으로 시작한다.** `GetPrivateProfileIntW`/
`WritePrivateProfileStringW`는 이 파일을 ANSI로 취급하므로 BOM 3바이트가 첫 줄 앞에 붙어 **맨 처음
`[trainer]` 섹션 헤더가 섹션으로 인식되지 않는다.** 그래서 예전에 저장이 일어났을 때 파일 끝에
`[trainer]` 섹션이 한 번 더 붙었고, 지금은 그 두 번째 것만 유효하다 (Win32 API로 직접 읽어서
확인: `trainer/no_recoil = 1`, 즉 뒤쪽 값). 동작에 지금 당장 문제는 없지만 파일 앞부분 4줄이 죽은
텍스트다. BOM을 그냥 지우면 앞쪽 죽은 섹션이 되살아나 `no_recoil`이 1에서 0으로 뒤집히므로,
고칠 때는 **BOM 제거와 죽은 섹션 삭제를 반드시 같이** 해야 한다. 아마 이전 세션에서 PowerShell
`Set-Content`/`Out-File`(5.1은 UTF8에 BOM을 붙인다)로 이 파일을 만졌던 것이 원인이다.

## 2026-08-28 - 전체 계측 온 세션 실측 (9.5분, 인텐스 배틀): 다음 최적화 순서 확정

로깅·프로파일링·VEH를 전부 켜고(`logging=1 profiling=1 veh=1 dbgout=0`, async=on) 14:41에 주입,
약 14:43:30부터 세션 끝(14:50:58)까지 인텐스 배틀을 유지했다. 스냅샷 44~70명 구간의 표본이
처음으로 충분히 쌓였다 (로그 1,235줄, 264 KB — 비동기 writer 전환 후 첫 실전 세션).

### Present(렌더) 스레드 — 배틀 구간, 프레임당

| 슬롯 | avg | max | 비고 |
|---|---|---|---|
| esp | 108~194 us | ~1000 us | 스냅샷 1회 포함 |
| snapshot (1회) | 48~108 us | ~790 us | **표본 수가 esp의 정확히 2배 — Phase 1 이중 패스 그대로** |
| poseSlots | 7.5~8.4 us | 243 us | 프레임당 ~9.6회 = **프레임당 ~75 us**, snapshot 내부 |
| aimbot | 29~44 us | 689 us | |
| snapLockWait | 0.0 us | 1.6 us | 락 경합은 존재하지 않음 |

Present 빈도는 한산 구간 516회/초 → 배틀 116~160회/초 (게임 자체 부하). 트레이너 present 경로
총합은 프레임당 약 200 us(esp + aimbot + 두 번째 snapshot)로, 배틀 프레임 예산의 2~3% 수준.

### 메인 틱 — 배틀 구간, 틱당 (틱 ~120–150회/초)

| 슬롯 | avg | max | 비고 |
|---|---|---|---|
| tickTotal | 30~36 us | 235 us | |
| healthInvoke | 22~26 us | 205 us | **틱 총량의 75~80% — 이제 압도적 1위** |
| healthCollect | 0.1~0.2 us | 2.8 us | Phase 5 점유 비트맵, 퍼펫 70명에서도 유지 |
| attitude | 2.9~5.7 us | 70 us | **직전 세션의 미설명 38.7 us가 사라졌다** |
| visibility | 0.2~1.2 us | 180 us | |

attitude 수치는 포인터 캐시 revert(`4319ab7`) 이후 첫 실전 검증이다. 퍼펫 41명에서 40.7 us였던
것이 50~70명에서 3~6 us — resolve 호출 경로 교정이 현장에서 확인됐고, 갱신된 순서 1번은 완료로
간주한다.

### 카운터에서 새로 드러난 것

- **VEH 9.5분 내내 예외 0건.** 관측 비용도 문제도 없었지만 얻는 것도 없으니 기본 off 유지가 맞다.
- **visibility 요청 폭주:** 배틀 개시 직후 ~12초 동안 dropped가 2,082 → 10,838로 폭증
  (`processed=1 queued=63` — 큐 64가 포화). 그 뒤 세션 끝까지 신규 drop 0. 새 엔티티가 한꺼번에
  등장하는 구간에서 캐시 미스 엔티티마다 매 프레임 요청을 다시 넣는 것으로 보인다 — **in-flight
  중복 제거가 없다는 신호.** 정상 상태에선 큐가 완전히 비므로 예산(틱당 1캐스트) 자체는 부족하지 않다.
- health 조회 누적 524k valid / 79k invalid — **13%가 invalid 대상에게 나간 리플렉션 호출**이다.
  invalid로 판명된 엔티티에 백오프 없이 계속 재시도하는 것으로 보인다.

### 다음 최적화 순서 (이 세션 데이터 기준)

1. **Phase 1 — 스냅샷 패스 프레임당 1회 통합.** DrawOverlay가 한 번 떠서 aimbot에 배열을 넘긴다.
   절감은 싼 쪽 패스 ~30 us/frame. 위험 최저, 코드 두 곳(`esp.cpp:209`, `aimbot.cpp:114`).
2. **Phase 2 축소판 — 비싼 쪽 스냅샷 패스(~110 us) 다이어트.** 카테고리를 TrackPuppet 시점 캐시로,
   `IsCorpseDead` 주기 제한, `kMaxSkeletonSegments` 64→8. **Phase 5의 교훈 유지: 값은 캐시해도
   게임 객체 포인터는 틱 너머로 캐시하지 않는다.**
3. **healthInvoke 절감 — 메인 틱 1위 항목.** (a) invalid 대상 백오프(13% 즉시 회수),
   (b) 카테고리·거리·화면 내 여부로 조회 대상 게이팅. 스탯 풀 직접 읽기(리플렉션 제거)는 RE가
   필요하므로 (a)(b) 효과 실측 후 별도 판단.
4. **Phase 3 — poseSlots를 메인 틱으로 이전.** present 스레드 최대 단일 항목(~75 us/frame)이지만
   유일하게 스레드 컨텍스트를 옮기는 작업이라 1~3 검증 후 착수.
5. **visibility in-flight 중복 제거 + drop 원인별 카운터.** 예산 증액(Phase 4)은 중복 제거 후
   재측정으로 판단.

각 단계는 이번과 동일한 프로토콜(전체 계측 온 + 인텐스 배틀)로 전후 비교한다.

## 2026-08-28 - Phase 1: 스냅샷 패스를 프레임당 1회로 통합

`Esp::DrawOverlay`와 `Aimbot::RunFrame`이 각자 `GetPuppetSnapshots`를 부르던 것을 없애고,
`Features::DrawOverlay`가 프레임당 한 번 패스를 돈 뒤 결과 배열을 두 기능에 넘기도록 바꿨다.
헤드리스 진단 경로(`Features::UpdateHeadless`)도 같은 방식으로 한 번만 돈다.

- `Features::FrameSnapshots`(포인터 + 개수)를 `features.h`에 두고, 128칸 배열의 소유자는
  `features.cpp`로 옮겼다. 예전엔 esp.cpp와 aimbot.cpp에 128칸짜리 static 배열이 각각 있었다.
- `Esp::DrawOverlay(settings, frame)`, `Aimbot::DrawOverlay(settings, frame)`,
  `Aimbot::UpdateHeadless(settings, frame, w, h)`로 시그니처가 바뀌었다.
- 배열은 Present 스레드 전용이다. `Overlay::OnPresent`가 `g_renderMutex`를 잡은 뒤에만 이 경로로
  들어오므로 프레임당 한 번 채워지고 같은 프레임 안에서만 읽힌다.
- `UpdateNativeHighlights`는 원자 변수만 건드리므로(엔티티 리스트를 안 만짐) 스냅샷보다 뒤로 밀려도
  동작이 달라지지 않는다. `GetStats()`가 이제 직전 프레임이 아니라 이번 패스의 카운터를 읽는 것만 차이.

### 계측 슬롯 의미 변화 (전후 비교할 때 주의)

- `SnapshotPass` / `SnapshotLockWait` / `SnapshotPuppets`의 표본 수가 **`esp`의 2배에서 1배로**
  떨어져야 한다. 이게 Phase 1이 실제로 먹혔는지 보는 1차 지표다.
- `esp` 슬롯에는 더 이상 스냅샷 비용이 포함되지 않는다. 직전 세션의 esp 108~194 us는 스냅샷
  48~108 us를 품은 값이었으므로, 새 세션의 esp는 그만큼 낮게 찍히는 게 정상이다. 프레임 총량을
  비교하려면 `snapshot + esp + aimbot`을 합쳐서 봐야 한다.
- 기대 절감은 싼 쪽 패스 ~30 us/frame. `poseSlots`는 여전히 스냅샷 안에 있으므로 프레임당 호출
  횟수(~9.6회)도 절반 아래로 떨어질 수 있다 — 그건 Phase 2/3 판단에 쓴다.

빌드는 통과했다(`cmake --build build --config Release`, 경고 없음). 실측은 다음 세션에서 동일
프로토콜(전체 계측 온 + 인텐스 배틀)로 한다.

### 개발 환경 메모

`cmake`가 PATH에 없다. winget으로 깔린 실체는
`C:\Users\admin\AppData\Local\Microsoft\WinGet\Packages\Kitware.CMake_Microsoft.Winget.Source_8wekyb3d8bbwe\cmake-4.4.2-windows-x86_64\bin\cmake.exe`
이며, 이 절대 경로로 부르면 그대로 빌드된다 (`build/CMakeCache.txt`의 `CMAKE_COMMAND`와 동일).

### 프리징 인스턴스 정리

14:53 프리징 상태로 남아 있던 게임 프로세스(PID 26508)는 진단 데이터를 다 뽑은 뒤 종료했다.

## 2026-08-28 - Phase 1 실측 검증 (15:24~15:46 세션) + 15:46:10 크래시 분석

같은 로그 파일(`cp2077_trainer.log`) 안에 Phase 1 전(14:41~14:53)과 후(15:24~15:46) 세션이 나란히
있어서 동일 계측 설정(`logging=1 profiling=1 veh=1 dbgout=0`)으로 직접 비교했다. 후 세션은 22분,
profile 윈도우 254개.

### Phase 1은 들어갔다

`snapshot` 표본 수 / `esp` 표본 수 비율이 **모든 윈도우에서 정확히 2.00 → 1.00**으로 떨어졌다.
이중 패스는 사라졌다.

퍼펫 수로 버킷을 맞춘 Present 스레드 트레이너 총량(전=esp+aimbot, 각각 자기 패스를 품고 있었음 /
후=snapshot+esp+aimbot):

| 퍼펫 | 전 | 후 |
|---|---|---|
| 30~45 | 140.1 us | 121.0 us |
| 45~60 | 154.0 us | 155.6 us |
| 60~80 | 240.8 us | 194.0 us |

60~80 버킷을 분해하면 예측이 맞았다는 게 보인다.

- 전: 두 패스 합계 210.8 us/frame (비싼 쪽 ~177, 싼 쪽 ~35), esp+aimbot 자체 작업 ~30 us.
- 후: 단일 패스 167.0 us/frame, esp+aimbot 자체 작업 26.9 us.
- 즉 **없앤 것은 싼 쪽 패스 ~35 us/frame**. 예측이 ~30 us였으니 그대로다.

45~60 버킷만 이득이 안 보이는 이유는 `poseSlots` 때문이다. 프레임당 호출 수가 전 7.7회 / 후 9.4회로
달랐다. poseSlots는 **패스당이 아니라 퍼펫당 33 ms 게이트**라 패스를 반으로 줄여도 총량이 안 준다.
버킷 비교에서 통제되지 않는 유일한 큰 변수이므로, 앞으로 전후 비교할 때는 `poseSlots/frame`을 같이
봐야 한다.

### 병목이 이동했다 — 다음 순서를 바꿔야 한다

60~80 퍼펫 구간 기준으로:

| 항목 | 프레임당 | 비중 |
|---|---|---|
| snapshot (단일 패스) | 167.0 us | Present 총량의 86% |
| ㄴ 그중 poseSlots | 95 us (11.7회 x 8.0 us) | snapshot의 57% |
| esp | 18.0 us | |
| aimbot | 8.9 us | |

**`esp`와 `aimbot` 자체 작업은 이제 합쳐서 27 us로 잡음 수준이다.** 남은 것은 전부 스냅샷 패스이고,
그 절반 이상이 poseSlots다. 그래서 **poseSlots 이전(기존 Phase 3)이 Present 스레드 1순위로 올라가고,
기존 Phase 2(스냅샷 패스 다이어트)가 그 다음**이다. 순서를 이렇게 바꾼다:

1. poseSlots를 메인 틱으로 이전 (구 Phase 3) — Present 최대 단일 항목 ~95 us/frame.
2. 스냅샷 패스 나머지 다이어트 (구 Phase 2) — 카테고리 TrackPuppet 시점 캐시, `IsCorpseDead` 주기
   제한, `kMaxSkeletonSegments` 64→8. **값은 캐시해도 게임 객체 포인터는 틱 너머로 캐시하지 않는다.**
3. healthInvoke 절감 — 메인 틱은 그대로 1위다(아래).
4. visibility in-flight 중복 제거 — 이번 세션에 재현되지 않아 보류(아래).

### 메인 틱은 그대로

Phase 1은 메인 틱을 건드리지 않았고 수치도 그대로다. `tickTotal` 33~35 us 중 `healthInvoke`가
25~26 us로 **75~79%**. 세션 누적 health 조회는 valid 1,220,877 / invalid 146,057으로 **invalid가
10.7%** (직전 세션 13%). attitude는 valid 376,277 / invalid 26,392로 6.6%, 시간은 2.8~4.6 us로 안정.

### visibility drop은 이번엔 0

세션 전체 441개 tick 로그에서 `dropped=0`이 한 번도 안 깨졌다 (직전 세션은 최대 10,838). 다만 이건
Phase 1의 효과가 **아니다** — `Visibility::Query`는 esp 루프와 aimbot 루프에서 각각 불리고 그 횟수는
안 바뀌었다. 요청 폭주 상황 자체가 재현되지 않은 것으로 본다. 중복 제거 작업은 재현 조건을 다시 잡은
뒤에 판단한다.

### 15:46:10 크래시

- `CrashInfo.json`: `timeCrash=2026-08-28T06:46:10Z`(KST 15:46:10), `district=CharterHill`,
  `trackedQuest=q103_warhead / go_to_panam / distance=516`, `sessionLength=1428.10`, `isOom=false`.
- 트레이너 로그 마지막 줄이 15:46:10.372 — **같은 초**. 그 뒤 아무 것도 안 남았다.
- Windows Application 이벤트 없음, WER 덤프 없음, 트레이너 미니덤프 없음(`WriteMiniDump`는 선언만
  되어 있고 호출하는 곳이 없다). System 로그 15:40~15:55 구간엔 Game DVR DCOM 타임아웃뿐 —
  **TDR도 스토리지 이벤트도 없다.** (WER 큐의 `Kernel_141`은 같은 날 13:46:14 것으로 2시간 전이다.)
  예전 기록대로 CDPR 자체 예외 필터가 받아서 `CrashInfo.json`만 남기고 즉시 종료한 형태다.

#### VEH가 0건인 것은 아무것도 증명하지 않는다 (직전 세션 결론 정정)

직전 세션에서 "VEH 예외 0건"을 근거로 삼았는데, 지금 구조로는 **치명적 예외야말로 절대 기록될 수 없다.**
구멍이 두 개다:

1. `VectoredExceptionHandler`는 링 버퍼에만 적고, 실제 로그로 옮기는 `DrainExceptionLog()`는
   `HookOnTick`(`visibility.cpp:351`), 즉 **다음 메인 틱**에서 돈다. 프로세스를 죽이는 폴트는 그 다음
   틱이 오지 않는다.
2. 설령 drain까지 갔어도 `Diagnostics::Log`는 비동기다. writer 스레드는 1초 cadence로 flush하므로
   즉사하면 큐에 있던 줄이 통째로 날아간다.

즉 VEH는 "게임이 스스로 복구하는 예외"를 세는 데는 쓸 수 있지만 크래시 원인 규명에는 지금 그대로는
쓸 수 없다. **다음 작업 전에 이 구멍부터 막는 것을 권한다** (fatal 코드에 한해 미리 열어둔 핸들에
`WriteFile`로 직접 쓰기 — CRT·모듈 조회·할당·로더 락 없이 손으로 헥사만 찍는 방식).

#### 정황 (증거 아님)

마지막 ESP 진단 3초 사이에 추적 대상이 무너진다: `snapshots` 53 → 41, `hostile` 5 → 0,
`dead` 7 → 2. 목표 지점까지 516 m를 이동 중이었으니 스트리밍 언로드로 대량 디스폰이 일어나는
구간이었다는 뜻이다. Present 스레드 스냅샷 패스가 게임 스레드가 파괴 중인 엔티티 객체를 역참조하는
구조적 위험이 여기 그대로 걸린다. 다만 **Phase 1은 그 패스를 프레임당 2회에서 1회로 줄였으므로
노출을 늘리지 않는다.** 사일런트 에임은 15:45:29에 마지막으로 armed됐고 크래시까지 41초간 놀고 있었다.

## 2026-08-29 - 크래시 로그 공백 막기 (치명적 폴트 직기록)

8/28 15:46:10 크래시에서 우리 쪽 증거가 통째로 비어 있었다. 원인은 진단 설계 그 자체였다:

- `Log()`는 링 버퍼에 넣고 즉시 반환하고, 실제 쓰기는 writer 스레드가 1초 cadence로 한다.
  → 마지막 1초 분량이 프로세스와 함께 사라진다.
- VEH는 예외를 링에 적기만 하고, 그것을 사람이 읽을 로그로 옮기는 `DrainExceptionLog()`는
  **다음 메인 틱**에서 돌린다 (`visibility.cpp:351`). 프로세스를 죽이는 폴트에는 그 틱이 오지 않는다.
- `WriteMiniDump`는 존재하지만 **호출자가 하나도 없다**.

즉 "VEH 예외 0건"은 아무것도 증명하지 못했다. 치명적 폴트는 현재 계측이 구조적으로 기록할 수 없는
바로 그 경우였다.

### 무엇을 넣었나

`src/diagnostics.cpp`에 치명적 폴트 전용 기록 경로를 추가했다. 평시 로그 경로는 건드리지 않았다.

- 미리 열어 둔 전용 핸들(`cp2077_fatal.log`, `FILE_FLAG_WRITE_THROUGH`)에 링을 거치지 않고 직접 쓴다.
- 두 지점에서 기록한다.
  - **VEH (first-chance)** → `[FATAL][veh]`. 게임이 스스로 복구한 예외도 여기 찍힌다.
  - **`SetUnhandledExceptionFilter` (마지막 기회)** → `[FATAL][unhandled]`. 아무도 처리하지 않았다는
    뜻이므로 이 예외가 실제로 프로세스를 죽였다는 확정 신호다. 이전 필터(= CDPR post-mortem)를
    붙잡아 두고 반드시 이어서 부른다 — `CrashInfo.json`이 지금까지 유일한 증거였으므로 잃으면 안 된다.
  - UEF에서 `Flush()`도 부른다. 링에 남은 평시 로그의 마지막 1초를 살리는 것이 이 한 줄이다.
- 레코드 내용: 로컬 시각 + 원본 FILETIME, pid/tid, 예외 코드와 이름, 폴트 주소(모듈+오프셋),
  AV의 R/W와 대상 주소, **`fault-in-trainer=YES/no`**, GPR 전체, 그리고 스택 스캔.
- 세션 헤더에 모듈 표를 같이 적어서 이 파일 하나만으로 원시 주소를 풀 수 있게 했다.
- `[SESSION] closed cleanly` 줄이 없는 세션 = 정상 종료가 아니었다. 예외 레코드도 없이 그렇다면
  `TerminateProcess`/행/전원 차단을 가리킨다.

### 로거가 크래시·프리징의 원인이 되지 않게 한 방법

이 경로의 설계 목표는 그것 하나였고, 기록 품질은 전부 그 밑에 뒀다.

| 위험 | 대응 |
|---|---|
| CRT/로케일 락, 할당 | `snprintf` 안 쓴다. 16진/10진 포맷터를 손으로 썼다. 핸들러가 부르는 WinAPI는 `GetSystemTimeAsFileTime` / `GetCurrentThreadId` / `GetCurrentProcessId` / `WriteFile` / `GetCurrentThreadStackLimits` 뿐 |
| 로더 락 | 모듈 조회를 핸들러에서 하지 않는다. Initialize에서 Toolhelp으로 스냅샷을 떠 두고 정적 배열 선형 스캔으로 푼다. DbgHelp/`MiniDumpWriteDump`는 이 경로에서 쓰지 않는다 |
| 파일 열기(힙 락) | 핸들·타임존 보정값·모듈 표 전부 Initialize에서 준비 |
| **복구되는 AV마다 디스크를 만져 생기는 프리징** | 프로세스 수명 전체에 쓰기 예산 16줄. 소진 뒤에는 핸들러 비용이 1.3 µs로 떨어진다 |
| 잠금 대기 | 정적 버퍼 상호배제는 `InterlockedCompareExchange` try-lock 하나. 실패하면 기다리지 않고 포기 |
| 스택 오버플로우 | 스택 버퍼 대신 정적 버퍼 사용 (남은 스택이 한 페이지뿐인 상황) |
| 핸들러 안에서의 재폴트 | 전 구간 `__try`. 코어 레코드와 스택 스캔을 따로 써서 스캔이 폴트나도 코어는 남는다 |
| 스택 되감기 | `RtlLookupFunctionEntry`/`StackWalk64`는 락을 잡을 수 있어 쓰지 않는다. `GetCurrentThreadStackLimits`로 경계를 잡고 RSP 위쪽을 훑어 모듈 범위에 떨어지는 값만 적는다 |

### 검증 — 게임 없이 이 경로만 격리해 돌렸다

`diagnostics.cpp` + `profiling.cpp`를 그대로 링크한 테스트 하네스를 만들어(스크래치패드, 커밋 안 함)
네 시나리오를 돌렸다. 전부 통과:

| 시나리오 | 결과 |
|---|---|
| 복구되는 AV 40회 | 레코드 정확히 16줄(예산). 예산 소진 후 핸들러 비용 **1.3 µs**, 예산 내 ~200 µs |
| 8스레드 × 50 폴트 (동시 400회) | 데드락 없음(`WaitForMultipleObjects` = 0), 레코드 16줄 |
| 스택 오버플로우 | `0xC00000FD` 레코드 기록, 프로세스가 살아 돌아옴 |
| 미처리 폴트 | `[veh]` + `[unhandled]` 두 줄, `closed cleanly` 없음, **링에만 있던 마지막 로그 줄이 살아남음** |

최악의 경우 디스크 비용은 세션 전체에서 16 × ~200 µs ≈ 3.2 ms다.

하네스가 실제 버그 두 개를 잡았다. 둘 다 이 검증이 없었으면 게임에서 처음 크래시 날 때 알았을 것이다.

1. **스택 스캔이 통째로 사라졌다.** RSP + 768바이트가 스레드 스택 꼭대기를 넘어가 폴트 →
   바깥 `__except`가 그때까지 모은 줄까지 삼켰다. `GetCurrentThreadStackLimits`로 클램프하고,
   읽기 루프만 따로 감싸 부분 출력이 남게 고쳤다.
2. **동시 폴트에서 예산만 타고 기록은 1줄.** 예산을 게이트보다 **먼저** 깎아서, 게이트에서 밀린
   스레드가 아무것도 쓰지 않고 예산을 소모했다. 순서를 뒤집었다 (400회 동시 폴트에서 1줄 → 16줄).

### 알아 둘 동작 변화 하나

`fatal_log` 기본값이 1이고, 이것이 켜져 있으면 **`veh=0`이어도 VEH가 등록된다**. first-chance를 볼
통로가 그것뿐이고, 특히 `__fastfail`(`STATUS_STACK_BUFFER_OVERRUN`)은 UEF를 아예 건너뛰기 때문이다.
VEH는 기본 off였고 그 이유(프로세스 전체 예외 경로에 끼어든다)는 여전히 유효하지만, 판단 근거는:

- 8/28 전체 계측 세션 12분간 치명적 클래스 예외가 **0건**이었다. 노이즈로 예산이 타지 않는다.
- 비치명 예외에서 핸들러 비용은 null 검사 + switch + return이다.

옛 동작으로 완전히 되돌리려면 `fatal_log=0` (또는 `CBPK_FATAL=0`) 하나면 된다. 초기화 로그의
`fatal sink:` / `diagnostics toggles:` 줄에 그 세션의 상태가 남는다.

### 다음

최적화는 재정렬된 순서 그대로다: (1) poseSlots → 메인 틱, (2) 스냅샷 패스 다이어트,
(3) healthInvoke 축소, (4) visibility in-flight 중복 제거(재현 대기). 다음 세션에서는
`cp2077_fatal.log`도 같이 회수할 것.

## 2026-08-29 20:52 - 크래시 로거 실전 검증 + 슬롯 조회 접기

### 1. 크래시 로거를 실제 게임에서 검증했다

20:39:49에 현재 빌드가 주입된 PID 12876에서 확인:

```
fatal sink: armed path=...\cp2077_fatal.log budget=16 modules=190 unhandled-filter=chained
diagnostics toggles: logging=1 profiling=1 veh=1 dbgout=0 fatal=1
```

**13분간 치명 클래스 예외 0건.** 예산이 노이즈로 타지 않는다는 실전 확인이다(하네스가 아니라 실제
전투/이동 중 측정).

그 뒤 의도적으로 폴트를 냈다. 트레이너 DLL의 PE 헤더(읽기 전용·비실행)를 시작 주소로 원격 스레드를
띄우면 명령어 인출이 DEP를 때리므로, 폴트가 **우리 모듈 안에서** 나면서 모듈 해석과
`fault-in-trainer` 판정까지 한 번에 검증된다. 결과 (`logs_archive/cp2077_fatal_20260829_2052_crash.log`):

```
[FATAL][veh]       20:52:59.223 ... at=0x00007FFBD57E0000 (cp2077_trainer.dll+0x0)
                   access=EXECUTE(DEP) fault-in-trainer=YES
                   stack: KERNEL32.DLL+0x2CCB7 / ntdll.dll+0xAAD6C
[FATAL][unhandled] 20:52:59.241 ... (같은 폴트, 18 ms 뒤)
                   stack: ... + KERNELBASE.dll+0x10F420
```

- 두 레코드가 모두 남았다 → first-chance와 "실제로 죽였다"가 구분된다.
- 190개 모듈 스냅샷으로 주소가 전부 이름+오프셋으로 풀렸다.
- `[SESSION] closed cleanly` 없음 → 비정상 종료로 올바르게 표시됐다.
- 프로세스는 죽었고, 로거가 프리징을 만들지 않았다.

**남은 미확인 하나**: UEF의 `Flush()`가 이번 실전에서 실제로 뭔가를 건졌는지는 확정할 수 없다.
직전 로그 줄이 20:52:57.181이고 다음 cadence 배치가 20:52:59.24로 폴트와 사실상 동시였다.
하네스에서는 결정적 마커로 확인된 동작이므로 기전 자체는 검증됐지만, 실전 증거는 다음 크래시로 미룬다.

### 2. 성능 지표 (20:39 세션, 157 윈도우 / 127,951 프레임)

Phase 1 적용본 기준. 퍼펫 수로 버킷팅한 Present 스레드 비용:

| 퍼펫 | 윈도우 | 프레임 | snapshot | pose/프레임 | poseCost | pose 비중 | esp | aimbot |
|---|---|---|---|---|---|---|---|---|
| 20–30 | 95 | 72,582 | 74.8 µs | 3.85 | 29.7 µs | 40% | 10.0 µs | 8.3 µs |
| 30–45 | 19 | 12,819 | 97.1 µs | 5.55 | 43.6 µs | 45% | 13.7 µs | 9.0 µs |
| 45–60 | 20 | 13,842 | 128.5 µs | 8.44 | 66.9 µs | 52% | 13.3 µs | 8.8 µs |
| 60–80 | 13 | 9,769 | 141.7 µs | 9.23 | 75.1 µs | 53% | 11.8 µs | 9.4 µs |
| 80 | 3 | 3,238 | 285.4 µs | 7.81 | 43.4 µs | 15% | 7.2 µs | 7.3 µs |

- **pose가 스냅샷 패스의 40~53%**로 확정. 프레임 대부분(72.5k/128k)이 몰린 20–30 구간에서도 40%다.
- 80퍼펫 구간만 다르다: 285 µs 중 pose는 43 µs뿐이고 나머지 ~240 µs가 다른 곳이다. 퍼펫당 비pose
  비용이 3.0 µs로 다른 구간(1.0~1.8 µs)의 2~3배다. **아직 정체를 모른다** — 스냅샷 패스 다이어트를
  할 때 여기부터 볼 것.
- `snapshot max`가 878~1263 µs, 한 윈도우는 15,526 µs까지 튄다. 평균이 아니라 이 스파이크가 체감
  히칭의 정체일 가능성이 있다. 별도로 추적할 것.

ESP 진단이 보여주는 깔때기: `snapshots=28 → projected=14 → front=6 → withinDistance=5 → drawn=4`.
**pose가 필요한 대상은 4~6인데 28개 전부에 대해 계산하고 있다.** 이건 다음 단계 후보다(아래 참고).

### 3. 이번에 넣은 것: 슬롯 조회 접기

`ReadCurrentPoseSlots`는 슬롯 6개(Head/Chest/Hips/RightHand/LegLeft/LegRight)를 읽는데, 예전
`ReadSlotPosition`은 **슬롯 이름마다** 이 일을 전부 다시 했다:

1. 컴포넌트 리스트 전체 순회
2. 컴포넌트마다 `IsClassOrDerived(entSlotComponent)` RTTI 계층 검사
3. `FindFunction(GetSlotTransform)` 조회
4. `Invoke`

1~3은 슬롯 이름과 무관한데 6번 반복됐다. 게다가 조기 종료 조건이 "그 이름으로 성공했을 때"라,
**해당 NPC에 없는 슬롯 이름에서는 매번 컴포넌트 전체를 끝까지 훑었다** (LegLeft/LegRight가 없는
NPC가 있다).

`FindSlotAccessors`로 슬롯 컴포넌트+함수를 **엔티티당 한 번** 찾아 두고, 6번은 `Invoke`만 남겼다.
하나만 찾고 끝내지 않고 목록(최대 4개)으로 들고 있는 이유는 **예전 동작을 그대로 보존하기 위해서**다.
예전 코드는 슬롯 컴포넌트가 둘 이상이면 이름마다 다른 컴포넌트가 뽑힐 수 있었고, 목록을 순서대로
시도하면 그 결과가 동일하다.

`entity_tracker.cpp` 안에서만 쓰이는 함수라 변경 범위가 파일 하나로 닫힌다. `PoseSlots` 계측 범위도
그대로 두어 전후 비교가 같은 기준으로 된다.

**검증 방법**: 다음 세션에서 같은 프로토콜(전체 계측 + 인텐스 배틀)로 `poseSlots avg`를 비교한다.
동시에 ESP 진단의 `poseBounds` / `skeletonLines` / `posePointCount`가 이전과 같은지 확인할 것 —
값이 줄었다면 슬롯 컴포넌트 선택 의미가 바뀐 것이므로 되돌려야 한다.

### 4. 다음 순서 (갱신)

1. **(이번 커밋) 슬롯 조회 접기** — 측정 대기
2. **pose를 실제로 그릴 대상에만** — 깔때기가 28 → 4~6이므로 여기가 가장 크다. 다만 화면에 새로
   들어온 퍼펫의 pose가 최대 33 ms 늦게 붙는 팝이 생길 수 있어, 체감 확인이 필요하다.
3. **poseSlots → 메인 틱** — 성능보다 **스레드 규칙 준수**가 본질이다. `ReadSlotPosition`은
   `Game::Rtti::Invoke`를 부르는데 지금 이것이 Present 스레드에서 돈다. AGENTS.md의 "스크립트 VM
   호출은 게임 메인 틱에서만" 규칙과 어긋난다. 1~2로 총량을 줄인 뒤에 옮기는 것이 옮길 양이 적다.
4. 스냅샷 패스 다이어트 — 특히 80퍼펫 구간의 정체 불명 240 µs
5. healthInvoke 축소 (tickTotal 28.8 µs 중 22.9 µs)
6. visibility in-flight 중복 제거 (재현 대기, 이번 세션도 `dropped=0`)

## 2026-08-30

- **라이브 프로세스(PID 29324) 프리징 사고 분석 및 보고서 작성**:
  - 인게임 구동 중 발생한 하드 프리징에 대해 Live 프로세스 메모리 덤프(`build/cp2077_crash_pid29324.dmp`, 158MB) 생성 및 MO2 Overwrite/트레이너 로그 역추적 완료.
  - 원인: 메인 틱 스레드(`TID 14916`)의 `FindAttitudeAgent`에서 RTTI 타입 검사(`IsClassOrDerived`) 중 유효하지 않은 포인터(`0xFFFFFFFFFFFFFFFF`) 역참조로 `0xC0000005` 발생.
  - `ReadHostility`에 `__try/__except`가 있었으나, 1st-chance 단계에서 REDengine 자체 VEH가 개입하여 크래시 덤프 매니페스트(`Cyberpunk2077.exe-*-14916.txt`)를 작성하고 메인 스레드를 정지시켜 전체 스레드 하드 프리징 유발.
  - `reports/TEMPLATE.md` 보고서 템플릿(메타데이터 헤더, 자율 본문, 후속 디버깅 힌트) 및 `reports/2026-08-30_freeze_analysis_pid29324.md` 분석 보고서 작성.
- **RTTI 역참조 포인터 정합성 검사 및 SEH 방어 강화 (Issue #1 패치)**:
  - `src/game/rtti_invoker.cpp`: `IsValidUserPointer` 검증 함수를 도입하고 `NativeType`, `IsClassOrDerived`, `FindFunction`, `ParentClass`, `ClassNameHash`, `FunctionCount`, `FunctionAt`, `ParameterCount`, `Invoke` 전반에 유저 모드 주소 범위(`0x10000 ~ 0x7FFFFFFEFFFF`) 검사 및 `__try / __except` 방어 블록 적용.
  - `src/game/entity_tracker.cpp`: `ForEachComponent`에서 엔티티/컴포넌트 엔트리/컴포넌트 인스턴스 포인터의 유효 범위를 선행 검사하도록 강화하고, 중복/불안전했던 로컬 `IsClassOrDerived`를 제거하고 `IsCorpseDead`가 안전한 `Game::Rtti`를 경유하도록 수정.
  - Release 빌드 정상 통과 (`cp2077_trainer.dll`).



## 2026-08-30 - PID 29324 프리징 수정 검토 + SEH 범위 축소 + 다음 최적화 계획

`945d596`(보고서)와 `f39e03f`(수정)를 검토했다.

### 1. 이번 변경이 원인인가 — 아니다

크래시 경로는 `HookOnTick` → `OnGameMainTick` → `ProcessAttitudeOnMainTick`(1520행) →
`ReadHostility`(1455행) → `FindAttitudeAgent`(1349행) → `Rtti::IsClassOrDerived`이다.
**게임 메인 틱의 attitude 경로**다.

직전 커밋 `719cf48`(슬롯 조회 접기)이 건드린 것은 528~640행 구간의 `FindSlotAccessors` /
`ReadSlotPosition` / `ReadCurrentPoseSlots`뿐이고, 이것은 **Present 스레드의 pose 경로**다.
스레드도 호출 경로도 겹치지 않는다. `198c4d6`(크래시 로거)도 VEH를 우선순위 0(마지막)으로 등록하고
`EXCEPTION_CONTINUE_SEARCH`만 반환하므로 예외 디스패치를 바꾸지 않는다.

오히려 반대다. **이 버그가 정확히 짚인 것은 크래시 로거 덕분이다.** `[FATAL][veh]` 레코드가
`at=cp2077_trainer.dll+0xD65C` / `fault-in-trainer=YES`를 남겼고, 그 RVA를 PDB로 풀어 소스 라인이
바로 나왔다. 로거를 넣기 전이었다면 CDPR 매니페스트의 "Unhandled exception" 한 줄만 남았을 것이다.

`4319ab7`에서 attitude 컴포넌트 포인터 캐시를 걷어낸 뒤로 `FindAttitudeAgent`가 **매 틱** 컴포넌트
목록을 새로 걷는다. 노출 빈도를 올린 것은 그 커밋이지, 이번 것들이 아니다.

### 2. 적용된 수정에서 좋았던 것

- `IsValidUserPointer` 선행 검증. 관측된 폴트를 실제로 막는 유일한 부분이다.
- entity_tracker.cpp의 **중복 `IsClassOrDerived` 로컬 사본 제거**와 `IsCorpseDead`를 가드가 있는
  `Game::Rtti` 쪽으로 돌린 것. 방어가 없던 사본이 하나 사라졌다.
- 보고서의 덤프 + PDB 심볼화 절차 자체는 재현 가능하고 유용하다.

### 3. 되돌린 것 두 가지 (이번 커밋)

**(a) `Rtti::Invoke`가 엔진 스크립트 VM 호출까지 `__try`로 감싸고 있었다.**
`execute(...)`는 게임의 `InternalExecute`다. 그 안에서 난 예외를 우리가 `EXCEPTION_EXECUTE_HANDLER`로
삼키면 VM은 **프레임이 반쯤 실행되고 refcount가 어긋난 상태로** 남는다. 지금 당장 안 죽어도 나중에
프리징이나 메모리 손상으로 돌아오는 모양이고, 우리가 없애려는 장애 그 자체다. 게다가 이번 폴트는
`execute` 안이 아니라 `IsClassOrDerived`에서 났으므로 이 확장은 보고된 버그와 무관하다.
→ 마샬링만 `BuildInvocationFrame`으로 떼어 SEH로 감싸고, **엔진 호출은 SEH 밖으로** 되돌렸다.
검증 로직은 하나도 빼지 않았다.

**(b) `ForEachComponent`의 `__except`가 콜백에서 난 예외까지 삼키고 있었다.**
containment 경계가 바뀐다. 예전에는 컴포넌트 순회 중 폴트가 `TrySnapshot`의 `__except`까지 올라가
`SnapshotResult::Stale` → 추적 목록에서 정리였다. 바뀐 코드에서는 `ForEachComponent`가 삼켜 버려
`TrySnapshot`이 **`Ready`를 반환**한다. 죽어 가는 엔티티가 부분 데이터를 들고 목록에 계속 남는다.
→ 게임 메모리를 읽는 부분만 `CollectComponents`로 떼어 SEH로 감싸고, **콜백은 SEH 밖에서** 부르게
되돌렸다. 덤으로 템플릿 본문에서 `__try`가 빠져 앞으로 소멸자 있는 캡처를 쓰는 콜백에서 C2712가
나지 않는다.

`IsValidUserPointer`는 `rtti_invoker.h`로 올려 두 파일이 같은 술어를 쓰게 했다. 상수 사본이 갈라지면
그 자체가 버그다.

### 4. 보고서에서 바로잡을 사실 두 가지

- **"SEH로 1st-chance VEH 크래시를 막는다"는 성립하지 않는다.** 커밋 메시지가 그렇게 되어 있는데,
  보고서 본문 스스로가 "VEH가 `__except`보다 먼저 돈다"고 옳게 적고 있다. `__try/__except`는
  1st-chance 디스패치 자체를 막지 못한다. **막는 것은 선행 검증뿐이다.** 앞으로 SEH를 방어책으로
  오해하지 않도록 남긴다.
- **`target=0xFFFFFFFFFFFFFFFF`는 센티넬 포인터 값이 아니다.** x64에서 non-canonical 주소 접근은
  #GP이고 Windows는 그때 폴트 주소를 `-1`로 보고한다. 즉 실제 포인터는 "0xFFFF...FF"가 아니라
  임의의 쓰레기 값이었다. 결과적으로 `IsValidUserPointer`가 걸러 주긴 하지만, 이는 **범위 검사이지
  유효성 검사가 아니다.** 이미 해제됐지만 주소만 그럴듯한 포인터는 그대로 통과해 폴트를 낸다.
  → 1st-chance 예외의 빈도를 줄일 뿐 없애지 못한다. 재발하면 다음은 소유권/수명 쪽을 봐야 한다.

### 5. 아직 측정하지 않은 것

`ForEachComponent`와 `IsClassOrDerived`는 직전 세션에서 **가장 비싼 경로로 측정된 바로 그 코드**다
(스냅샷 패스의 40~53%). 거기에 범위 검사와 SEH 프레임이 들어갔는데 전후 측정이 없다. 범위 검사는
비교 두 번이라 저렴하고 x64 SEH는 예외가 없으면 런타임 비용이 0이지만, 컴파일러가 가드 구간에서
지역 변수를 메모리에 묶는 영향은 남는다. **다음 세션에서 `poseSlots avg`로 같이 확인한다.**

### 6. 다음 최적화 계획

측정 대기 중인 것이 두 개 겹쳐 있다(슬롯 조회 접기, RTTI 검증 추가). 둘 다 같은 슬롯(`poseSlots`,
`snapshot`)에 나타나므로 **다음 세션 한 번으로 합쳐 확인**하고, 회귀가 보이면 그때 분리한다.

| # | 항목 | 근거 | 위험 |
|---|---|---|---|
| 1 | (측정) 슬롯 조회 접기 + RTTI 검증 | `poseSlots avg`, `snapshot avg`. `poseBounds`/`skeletonLines`가 이전과 같은지 함께 확인 | - |
| 2 | pose를 실제로 그릴 대상에만 | ESP 깔때기가 `snapshots=28 → drawn=4`. pose가 필요한 건 4~6인데 28개 전부 계산 중 | 화면에 새로 들어온 퍼펫의 pose가 최대 33 ms 늦게 붙는 팝 |
| 3 | poseSlots → 메인 틱 | **성능이 아니라 스레드 규칙 준수.** `ReadSlotPosition`이 `Rtti::Invoke`(스크립트 VM)를 Present 스레드에서 부른다 | 스레드 문맥 이동. 1~2로 총량을 줄인 뒤가 옮길 양이 적다 |
| 4 | 80퍼펫 구간 정체 불명 240 µs | 285 µs 중 pose는 43 µs뿐. 퍼펫당 비pose 비용이 다른 구간의 2~3배 | 원인 미상이라 먼저 계측 추가 |
| 5 | `snapshot max` 스파이크 | 878~1263 µs, 한 윈도우는 15,526 µs. 평균이 아니라 이쪽이 체감 히칭일 수 있다 | - |
| 6 | healthInvoke 축소 | tickTotal 28.8 µs 중 22.9 µs | - |
| 7 | visibility in-flight 중복 제거 | 두 세션 연속 `dropped=0` | 재현 대기 |

attitude 경로의 stale 포인터는 별도 항목으로 남긴다. 이번 수정은 빈도를 낮춘 것이지 없앤 것이 아니므로,
`cp2077_fatal.log`에 `FindAttitudeAgent` 계열 레코드가 또 나오면 수명 관리 쪽으로 접근할 것.

## 2026-08-30 - 라이브 프로세스(PID 13248) 프리징 사고 분석 및 아티팩트 보존

- **프리징 사고 개요**:
  - PID 13248 구동 약 71분(uptime 4298초) 시점인 01:48:03(KST)에 게임 화면 및 입력이 멈추는 하드 프리징 발생.
  - Live 프로세스 메모리 덤프(`build/cp2077_freeze_pid13248.dmp` / `reports/artifacts/2026-08-30_freeze_pid13248/cp2077_freeze_pid13248.dmp`, 160MB) 확보 완료.
  - 관련 아티팩트(`cp2077_fatal.log`, `cp2077_trainer.log`, `Cyberpunk2077.exe-20260830-003624-13248-30224.txt`, `.dmp`)를 `reports/artifacts/2026-08-30_freeze_pid13248/` 디렉터리에 격리 보존.
  - 분석 완료 후 프로세스 강제 종료(`Stop-Process -Id 13248 -Force`) 완료.
- **원인 분석 결과**:
  - 메인 틱 스레드(`TID 30224`)의 `FindAttitudeAgent` → `NativeType`(`rtti_invoker.cpp:152`)에서 해제된 힙 메모리 포인터(`0x41E5BB48`)의 클래스 오프셋(`+0x30`, `0x41E5BB78`) 역참조 시 `0xC0000005` 발생.
  - 이전 사고(PID 29324)와 **동일한 근본 구조적 원인**의 연속 발현. `IsValidUserPointer`는 유저모드 canonical 범위(`0x10000~0x7FFFFFFEFFFF`)만 검사하므로 Use-After-Free 포인터를 걸러내지 못하며, `__try/__except` 블록 도달 전에 엔진 VEH가 1st-chance로 예외를 낚아채 크래시 리포팅 및 메인 틱 스레드 정지 루프로 진입시켜 전체 스레드 하드 프리징 유발.
- **산출물**:
  - 분석 보고서: `reports/2026-08-30_freeze_analysis_pid13248.md`
  - 아티팩트 디렉터리: `reports/artifacts/2026-08-30_freeze_pid13248/`

