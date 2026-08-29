#pragma once

#include <cstddef>
#include <cstdint>

namespace Game::Rtti
{
    struct Class;
    struct Function;

    struct FunctionInfo
    {
        std::uint64_t fullNameHash = 0;
        std::uint64_t shortNameHash = 0;
        const Class* parent = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t registrationIndex = 0;
        std::size_t parameterCount = 0;
        bool hasReturnValue = false;
        void* nativeHandler = nullptr;
    };

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

    // 게임 소유 포인터가 역참조해도 될 만한 값인지 보는 최소 검사. 커널 주소와 non-canonical 주소를
    // 걸러낸다. x64에서 non-canonical 주소 접근은 #GP라 Windows가 폴트 주소를 0xFFFFFFFFFFFFFFFF로
    // 보고하는데, 2026-08-30 프리징의 예외 레코드가 정확히 그 모양이었다.
    //
    // 이것은 범위 검사이지 유효성 검사가 아니다. 이미 해제됐지만 주소만 그럴듯한 포인터는 그대로
    // 통과해서 폴트를 낸다. 1st-chance 예외의 빈도를 줄일 뿐 없애지는 못한다.
    inline bool IsValidUserPointer(const void* pointer) noexcept
    {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return address >= 0x10000ull && address <= 0x00007FFFFFFEFFFFull;
    }

    Class* NativeType(const void* object);
    bool IsClassOrDerived(const Class* type, std::uint64_t nameHash);
    Function* FindFunction(const Class* type, std::uint64_t functionNameHash);
    const char* ResolveName(std::uint64_t nameHash);
    bool InspectFunction(const Function* function, FunctionInfo& output);

    // Walks a single class in the inheritance chain. Discovery diagnostics use these to print the reflected
    // surface of a type when a name lookup fails, so a wrong guess is reported instead of silently disabling
    // a feature.
    const Class* ParentClass(const Class* type);
    std::uint64_t ClassNameHash(const Class* type);
    std::size_t FunctionCount(const Class* type);
    Function* FunctionAt(const Class* type, std::size_t index);

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
