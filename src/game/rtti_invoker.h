#pragma once

#include <cstddef>
#include <cstdint>

namespace Game::Rtti
{
    struct Class;
    struct Function;

    // The game uses a two-pointer handle for ref<T>/wref<T>.  Keep this
    // deliberately opaque to callers: it is only exposed so reflected
    // functions can receive the same storage layout as the script VM.
    struct Handle
    {
        void* instance = nullptr;
        void* refCount = nullptr;
    };
    static_assert(sizeof(Handle) == 0x10);

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

    // Minimal wrappers around the engine RTTI services used by main-tick
    // feature code.  These resolve through RED4ext and are safe no-ops when
    // the resolver is unavailable.
    Class* GetClass(std::uint64_t nameHash);
    std::size_t ClassSize(const Class* type);
    void* CreateInstance(Class* type);
    bool ConstructHandle(Handle* handle, void* instance);
    // Drops only a demonstrably non-final strong reference. The SDK's final Handle destructor also calls the
    // object's CanBeDestructed()/Memory::Delete path, whose ABI is not exposed here; final refs are retained.
    void ReleaseHandle(Handle* handle);
    bool HasReturnValue(const Function* function);

    // Reflected parameter count, including optional and out parameters. Invoke() requires an exact match, so
    // callers that hardcode a signature can verify it against the running build first.
    std::size_t ParameterCount(const Function* function);

    // Invokes a reflected REDengine function using its own parameter type descriptors.
    // The caller owns argument/result storage and must keep it alive for the duration of the call.
    bool Invoke(Function* function, void* context, const Argument* arguments, std::size_t argumentCount,
                void* result = nullptr);
}
