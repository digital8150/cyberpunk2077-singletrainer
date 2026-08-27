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
