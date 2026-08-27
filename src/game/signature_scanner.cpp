#include "signature_scanner.h"

#include <cstring>

namespace Game::Signatures
{
    ScanResult FindInText(HMODULE module, const std::uint8_t* pattern, const char* mask, std::size_t length)
    {
        ScanResult result;
        if (!module || !pattern || !mask || length == 0)
            return result;

        auto* imageBase = reinterpret_cast<std::uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return result;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return result;

        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (std::memcmp(section->Name, ".text", 5) != 0)
                continue;

            std::uint8_t* begin = imageBase + section->VirtualAddress;
            const std::size_t size = section->Misc.VirtualSize;
            if (size < length)
                return result;

            for (std::size_t offset = 0; offset <= size - length; ++offset)
            {
                bool matched = true;
                for (std::size_t byte = 0; byte < length; ++byte)
                {
                    if (mask[byte] != '?' && begin[offset + byte] != pattern[byte])
                    {
                        matched = false;
                        break;
                    }
                }

                if (!matched)
                    continue;

                ++result.matches;
                if (result.matches == 1)
                    result.address = begin + offset;
            }
            return result;
        }

        return result;
    }
}
