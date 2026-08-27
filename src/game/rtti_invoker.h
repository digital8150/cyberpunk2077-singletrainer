#pragma once

#include <cstddef>
#include <cstdint>

namespace Game::Rtti
{
    struct Class;
    struct Function;

    struct Argument
    {
        void* value = nullptr;
    };

    constexpr std::uint64_t Hash(const char* text)
    {
        std::uint64_t hash = 0xCBF29CE484222325ull;
        while (*text)
        {
            hash ^= static_cast<std::uint8_t>(*text++);
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    Class* NativeType(const void* object);
    bool IsClassOrDerived(const Class* type, std::uint64_t nameHash);
    Function* FindFunction(const Class* type, std::uint64_t functionNameHash);

    // Invokes a reflected REDengine function using its own parameter type descriptors.
    // The caller owns argument/result storage and must keep it alive for the duration of the call.
    bool Invoke(Function* function, void* context, const Argument* arguments, std::size_t argumentCount,
                void* result = nullptr);
}
