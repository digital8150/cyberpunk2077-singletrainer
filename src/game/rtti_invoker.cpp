#include "rtti_invoker.h"
#include "../diagnostics.h"
#include "../framework.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace
{
    constexpr std::uint32_t kInternalExecuteAddressHash = 0x1817231Du;
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
    };

    struct ClassLayout
    {
        std::byte pad00[0x10];
        ClassLayout* parent;
        std::uint64_t nameHash;
    };

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
    Class* NativeType(const void* object)
    {
        if (!object)
            return nullptr;
        return reinterpret_cast<Class*>(
            *reinterpret_cast<ClassLayout* const*>(static_cast<const std::byte*>(object) + 0x30));
    }

    bool IsClassOrDerived(const Class* type, std::uint64_t nameHash)
    {
        auto* current = reinterpret_cast<const ClassLayout*>(type);
        for (unsigned depth = 0; current && depth < 32; ++depth, current = current->parent)
        {
            if (current->nameHash == nameHash)
                return true;
        }
        return false;
    }

    Function* FindFunction(const Class* type, std::uint64_t functionNameHash)
    {
        auto* current = reinterpret_cast<const ClassLayout*>(type);
        for (unsigned depth = 0; current && depth < 32; ++depth, current = current->parent)
        {
            const auto* functions = reinterpret_cast<const DynArrayLayout*>(
                reinterpret_cast<const std::byte*>(current) + 0x48);
            if (!functions->entries || functions->size > functions->capacity || functions->size > 8192)
                continue;
            for (std::uint32_t i = 0; i < functions->size; ++i)
            {
                auto* function = static_cast<FunctionLayout*>(functions->entries[i]);
                if (function && function->shortNameHash == functionNameHash)
                    return reinterpret_cast<Function*>(function);
            }
        }
        return nullptr;
    }

    std::size_t ParameterCount(const Function* opaqueFunction)
    {
        const auto* function = reinterpret_cast<const FunctionLayout*>(opaqueFunction);
        if (!function || function->params.size > function->params.capacity || function->params.size > 24)
            return 0;
        return function->params.size;
    }

    bool Invoke(Function* opaqueFunction, void* context, const Argument* arguments, std::size_t argumentCount,
                void* result)
    {
        auto* function = reinterpret_cast<FunctionLayout*>(opaqueFunction);
        InternalExecuteFn execute = ResolveInternalExecute();
        if (!function || !execute || argumentCount > 24 ||
            (argumentCount > 0 && !arguments) || function->params.size != argumentCount ||
            function->params.size > function->params.capacity ||
            (function->params.size > 0 && !function->params.entries))
        {
            return false;
        }

        std::array<char, 512> bytecode{};
        char* cursor = bytecode.data();
        for (std::size_t i = 0; i < argumentCount; ++i)
        {
            auto* parameter = static_cast<PropertyLayout*>(function->params.entries[i]);
            if (!parameter || !parameter->type || !arguments[i].value)
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
        void* resultType = function->returnType ? function->returnType->type : nullptr;
        return execute(function, context, &frame, result, resultType);
    }
}
