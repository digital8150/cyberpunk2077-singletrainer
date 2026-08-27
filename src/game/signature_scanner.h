#pragma once

#include "../framework.h"

#include <cstddef>
#include <cstdint>

namespace Game::Signatures
{
    struct ScanResult
    {
        std::uint8_t* address = nullptr;
        std::size_t matches = 0;
    };

    // 로드된 PE 이미지의 .text 섹션만 검색한다. mask의 '?' 바이트는 wildcard다.
    ScanResult FindInText(HMODULE module, const std::uint8_t* pattern, const char* mask, std::size_t length);
}
