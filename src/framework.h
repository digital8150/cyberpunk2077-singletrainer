// 공통 include. 프로젝트 전역에서 이 헤더 하나로 Win32 + D3D12 타입을 끌어온다.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
