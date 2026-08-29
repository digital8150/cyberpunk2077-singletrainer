#include "rtti_invoker.h"
#include "../diagnostics.h"
#include "../framework.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>

namespace
{
    constexpr std::uint32_t kInternalExecuteAddressHash = 0x1817231Du;
    constexpr std::uint32_t kNativeFunctionHandlersAddressHash = 0x5A7D28A9u;
    constexpr std::uint32_t kCNamePoolGetAddressHash = 0x68DF07DCu;
    constexpr std::uint32_t kRttiSystemGetAddressHash = 0x4A610F64u;
    constexpr std::uint32_t kCClassCreateInstanceAddressHash = 0x5A800F1Du;
    constexpr std::uint32_t kHandleCtorAddressHash = 0xBA0C115Du;
    constexpr std::uint8_t kParamOpcode = 27;
    constexpr std::uint8_t kParamEndOpcode = 38;

    struct DynArrayLayout
    {
        void** entries;
        std::uint32_t capacity;
        std::uint32_t size;
    };

    struct PropertyLayout
    {
        void* type;
        std::uint64_t nameHash;
        std::uint64_t groupHash;
        void* parent;
        std::uint32_t valueOffset;
        std::uint32_t pad24;
        std::uint64_t flags;
    };

    struct FunctionLayout
    {
        void* vtable;
        std::uint64_t fullNameHash;
        std::uint64_t shortNameHash;
        PropertyLayout* returnType;
        std::uint64_t unk20;
        DynArrayLayout params;
        DynArrayLayout localVars;
        std::byte pad48[0xA8 - 0x48];
        std::uint32_t flags;
        std::uint32_t unkAC;
        void* parent;
        std::uint32_t regIndex;
        std::uint32_t padBC;
    };
    static_assert(offsetof(FunctionLayout, params) == 0x28);
    static_assert(offsetof(FunctionLayout, flags) == 0xA8);
    static_assert(offsetof(FunctionLayout, parent) == 0xB0);
    static_assert(offsetof(FunctionLayout, regIndex) == 0xB8);

    struct ClassLayout
    {
        std::byte pad00[0x10];
        ClassLayout* parent;
        std::uint64_t nameHash;
        std::byte pad20[0x68 - 0x20];
        std::uint32_t size;
    };

    static_assert(offsetof(ClassLayout, size) == 0x68);

    struct StackFrameLayout
    {
        char* code;
        FunctionLayout* function;
        void* localVars;
        void* params;
        std::int64_t unk20;
        std::int64_t unk28;
        void* data;
        void* dataType;
        void* context;
        StackFrameLayout* parent;
        std::int16_t unk50;
        std::byte pad52[6];
        std::int64_t unk58;
        std::uint16_t paramFlags;
        std::uint8_t currentParam;
        bool useDirectData;
        std::byte pad64[4];
    };
    static_assert(sizeof(StackFrameLayout) == 0x68);

    using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
    using InternalExecuteFn = bool (*)(FunctionLayout*, void*, StackFrameLayout*, void*, void*);
    using GetRttiSystemFn = void* (*)();
    // CClass::CreateInstance is the RED4ext SDK three-argument relocation: (class, size, zeroMemory).
    using CClassCreateInstanceFn = void* (*)(ClassLayout*, std::uint32_t, bool);
    using HandleCtorFn = void (*)(Game::Rtti::Handle*, void*);
    using CNamePoolGetFn = const char* (*)(const std::uint64_t&);

    void* VirtualFunction(void* object, std::size_t index)
    {
        if (!object)
            return nullptr;
        void** table = *reinterpret_cast<void***>(object);
        return table ? table[index] : nullptr;
    }

    ResolveAddressFn ResolveAddress()
    {
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        return red4ext ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                       : nullptr;
    }

    void* GetRttiSystem()
    {
        ResolveAddressFn resolve = ResolveAddress();
        const std::uintptr_t address = resolve ? resolve(kRttiSystemGetAddressHash) : 0;
        if (!address)
            return nullptr;
        return reinterpret_cast<GetRttiSystemFn>(address)();
    }

    InternalExecuteFn ResolveInternalExecute()
    {
        static bool attempted = false;
        static InternalExecuteFn execute = nullptr;
        if (attempted)
            return execute;
        attempted = true;

        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        if (resolve)
            execute = reinterpret_cast<InternalExecuteFn>(resolve(kInternalExecuteAddressHash));
        Diagnostics::Log("RTTI invocation resolver: InternalExecute=%p", reinterpret_cast<void*>(execute));
        return execute;
    }
}

namespace Game::Rtti
{
    [[nodiscard]] inline bool IsValidUserPointer(const void* ptr) noexcept
    {
        const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
        return addr >= 0x10000ULL && addr <= 0x00007FFFFFFEFFFFULL;
    }

    Class* NativeType(const void* object)
    {
        if (!IsValidUserPointer(object))
            return nullptr;
        __try
        {
            auto* classPtr = *reinterpret_cast<ClassLayout* const*>(
                static_cast<const std::byte*>(object) + 0x30);
            if (IsValidUserPointer(classPtr))
                return reinterpret_cast<Class*>(classPtr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return nullptr;
    }

    bool IsClassOrDerived(const Class* type, std::uint64_t nameHash)
    {
        if (!IsValidUserPointer(type))
            return false;
        __try
        {
            auto* current = reinterpret_cast<const ClassLayout*>(type);
            for (unsigned depth = 0; IsValidUserPointer(current) && depth < 32; ++depth, current = current->parent)
            {
                if (current->nameHash == nameHash)
                    return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return false;
    }

    Function* FindFunction(const Class* type, std::uint64_t functionNameHash)
    {
        if (!IsValidUserPointer(type))
            return nullptr;
        __try
        {
            auto* current = reinterpret_cast<const ClassLayout*>(type);
            for (unsigned depth = 0; IsValidUserPointer(current) && depth < 32; ++depth, current = current->parent)
            {
                const auto* functions = reinterpret_cast<const DynArrayLayout*>(
                    reinterpret_cast<const std::byte*>(current) + 0x48);
                if (!IsValidUserPointer(functions->entries) || functions->size > functions->capacity || functions->size > 8192)
                    continue;
                for (std::uint32_t i = 0; i < functions->size; ++i)
                {
                    auto* function = static_cast<FunctionLayout*>(functions->entries[i]);
                    if (IsValidUserPointer(function) && function->shortNameHash == functionNameHash)
                        return reinterpret_cast<Function*>(function);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return nullptr;
    }

    const Class* ParentClass(const Class* type)
    {
        if (!IsValidUserPointer(type))
            return nullptr;
        __try
        {
            const auto* parent = reinterpret_cast<const ClassLayout*>(type)->parent;
            return IsValidUserPointer(parent) ? reinterpret_cast<const Class*>(parent) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    std::uint64_t ClassNameHash(const Class* type)
    {
        if (!IsValidUserPointer(type))
            return 0;
        __try
        {
            return reinterpret_cast<const ClassLayout*>(type)->nameHash;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    std::size_t FunctionCount(const Class* type)
    {
        if (!IsValidUserPointer(type))
            return 0;
        __try
        {
            const auto* functions = reinterpret_cast<const DynArrayLayout*>(
                reinterpret_cast<const std::byte*>(type) + 0x48);
            if (!IsValidUserPointer(functions->entries) || functions->size > functions->capacity || functions->size > 8192)
                return 0;
            return functions->size;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    Function* FunctionAt(const Class* type, std::size_t index)
    {
        if (!IsValidUserPointer(type) || index >= FunctionCount(type))
            return nullptr;
        __try
        {
            const auto* functions = reinterpret_cast<const DynArrayLayout*>(
                reinterpret_cast<const std::byte*>(type) + 0x48);
            if (!IsValidUserPointer(functions->entries) || index >= functions->size)
                return nullptr;
            auto* func = static_cast<FunctionLayout*>(functions->entries[index]);
            return IsValidUserPointer(func) ? reinterpret_cast<Function*>(func) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    const char* ResolveName(std::uint64_t nameHash)
    {
        if (nameHash == 0)
            return "";
        __try
        {
            ResolveAddressFn resolve = ResolveAddress();
            const std::uintptr_t address = resolve ? resolve(kCNamePoolGetAddressHash) : 0;
            const char* result = address ? reinterpret_cast<CNamePoolGetFn>(address)(nameHash) : nullptr;
            return result ? result : "";
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return "";
        }
    }

    bool InspectFunction(const Function* opaqueFunction, FunctionInfo& output)
    {
        output = {};
        if (!opaqueFunction)
            return false;
        __try
        {
            const auto* function = reinterpret_cast<const FunctionLayout*>(opaqueFunction);
            if (function->params.size > function->params.capacity || function->params.size > 24)
                return false;

            output.fullNameHash = function->fullNameHash;
            output.shortNameHash = function->shortNameHash;
            output.parent = reinterpret_cast<const Class*>(function->parent);
            output.flags = function->flags;
            output.registrationIndex = function->regIndex;
            output.parameterCount = function->params.size;
            output.hasReturnValue = function->returnType != nullptr;

            const bool isNative = (function->flags & 1u) != 0;
            const bool isStatic = (function->flags & 2u) != 0;
            if (isNative && !isStatic && function->regIndex < 1000000u)
            {
                ResolveAddressFn resolve = ResolveAddress();
                const std::uintptr_t address = resolve ? resolve(kNativeFunctionHandlersAddressHash) : 0;
                // 2.31 InternalCallNative indexes the relocated address directly as Handler_t[regIndex].
                // It is the table base, not a pointer variable that needs another dereference.
                void** handlers = reinterpret_cast<void**>(address);
                if (handlers)
                    output.nativeHandler = handlers[function->regIndex];
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            output = {};
            return false;
        }
    }

    Class* GetClass(std::uint64_t nameHash)
    {
        __try
        {
            void* rttiSystem = GetRttiSystem();
            const auto getClass = reinterpret_cast<Class* (*)(void*, std::uint64_t)>(
                VirtualFunction(rttiSystem, 2));
            return getClass ? getClass(rttiSystem, nameHash) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    std::size_t ClassSize(const Class* opaqueType)
    {
        if (!opaqueType)
            return 0;
        __try
        {
            const auto* type = reinterpret_cast<const ClassLayout*>(opaqueType);
            return type->size;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void* CreateInstance(Class* opaqueType)
    {
        if (!opaqueType)
            return nullptr;
        __try
        {
            ResolveAddressFn resolve = ResolveAddress();
            const std::uintptr_t address = resolve ? resolve(kCClassCreateInstanceAddressHash) : 0;
            const std::size_t size = ClassSize(opaqueType);
            if (!address || size == 0 || size > (std::numeric_limits<std::uint32_t>::max)())
                return nullptr;
            return reinterpret_cast<CClassCreateInstanceFn>(address)(reinterpret_cast<ClassLayout*>(opaqueType),
                                                                       static_cast<std::uint32_t>(size), false);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool ConstructHandle(Handle* handle, void* instance)
    {
        if (!handle || !instance)
            return false;
        *handle = {};
        __try
        {
            ResolveAddressFn resolve = ResolveAddress();
            const std::uintptr_t address = resolve ? resolve(kHandleCtorAddressHash) : 0;
            if (!address)
                return false;
            reinterpret_cast<HandleCtorFn>(address)(handle, instance);
            return handle->instance == instance && handle->refCount != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *handle = {};
            return false;
        }
    }

    void ReleaseHandle(Handle* handle)
    {
        if (!handle || !handle->refCount)
            return;
        __try
        {
            // RED4ext::Handle::~Handle() calls RefCnt::DecRef(), then destroys the instance when that was the
            // final strong reference and CanBeDestructed() allows it. We do not have a verified ABI for either
            // CanBeDestructed() or Memory::Delete(), so never perform the final decrement here. Dropping a strong
            // reference is safe only when another strong owner is demonstrably present; use a CAS loop so a racing
            // owner cannot turn a >1 observation into an unsafe decrement-to-zero.
            struct RefCountLayout
            {
                volatile LONG strong;
                volatile LONG weak;
            };
            auto* refs = static_cast<RefCountLayout*>(handle->refCount);
            LONG observed = InterlockedCompareExchange(&refs->strong, 0, 0);
            while (observed > 1)
            {
                const LONG previous = InterlockedCompareExchange(&refs->strong, observed - 1, observed);
                if (previous == observed)
                    break;
                observed = previous;
            }
            // A strong count of one is the SDK's last-owner case and is deliberately retained. A zero count is
            // an expired/weak state, not a valid strong Handle destructor path; do not call DecWeakRef through a
            // guessed storage type. The caller's handle is cleared below so it cannot be reused accidentally.
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        *handle = {};
    }

    bool HasReturnValue(const Function* opaqueFunction)
    {
        if (!opaqueFunction)
            return false;
        __try
        {
            const auto* function = reinterpret_cast<const FunctionLayout*>(opaqueFunction);
            return function->returnType != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::size_t ParameterCount(const Function* opaqueFunction)
    {
        if (!IsValidUserPointer(opaqueFunction))
            return 0;
        __try
        {
            const auto* function = reinterpret_cast<const FunctionLayout*>(opaqueFunction);
            if (function->params.size > function->params.capacity || function->params.size > 24)
                return 0;
            return function->params.size;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    bool Invoke(Function* opaqueFunction, void* context, const Argument* arguments, std::size_t argumentCount,
                void* result)
    {
        if (!IsValidUserPointer(opaqueFunction) || !IsValidUserPointer(context))
            return false;

        __try
        {
            auto* function = reinterpret_cast<FunctionLayout*>(opaqueFunction);
            InternalExecuteFn execute = ResolveInternalExecute();
            if (!function || !execute || argumentCount > 24 ||
                (argumentCount > 0 && !arguments) || function->params.size != argumentCount ||
                function->params.size > function->params.capacity ||
                (function->params.size > 0 && (!IsValidUserPointer(function->params.entries) || !function->params.entries)))
            {
                return false;
            }

            std::array<char, 512> bytecode{};
            char* cursor = bytecode.data();
            for (std::size_t i = 0; i < argumentCount; ++i)
            {
                auto* parameter = static_cast<PropertyLayout*>(function->params.entries[i]);
                if (!IsValidUserPointer(parameter) || !IsValidUserPointer(parameter->type) || !arguments[i].value)
                    return false;
                *cursor++ = static_cast<char>(kParamOpcode);
                std::memcpy(cursor, &parameter->type, sizeof(parameter->type));
                cursor += sizeof(parameter->type);
                std::memcpy(cursor, &arguments[i].value, sizeof(arguments[i].value));
                cursor += sizeof(arguments[i].value);
            }
            *cursor = static_cast<char>(kParamEndOpcode);

            StackFrameLayout frame{};
            frame.code = bytecode.data();
            frame.function = function;
            frame.context = context;
            void* resultType = (function->returnType && IsValidUserPointer(function->returnType))
                                   ? function->returnType->type
                                   : nullptr;
            return execute(function, context, &frame, result, resultType);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}
