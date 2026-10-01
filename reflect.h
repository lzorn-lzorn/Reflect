#ifndef REFLECT_REFLECT_H
#define REFLECT_REFLECT_H
#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo> // std::bad_cast
#include <unordered_map>
#include <utility>
#include <vector>

#include "SerdeADT.h"

namespace core
{

struct ReflectNullBase {};

// ========================= 成员指针的类类型萃取 =========================
template<typename Ty> struct ReflectMemberPointerClass;
template<typename Class, typename Ty>
struct ReflectMemberPointerClass<Ty Class::*> { using type = Class; };
template<typename Ty>
using ReflectMemberPointerClass_t = typename ReflectMemberPointerClass<Ty>::type;

// ========================= 变量类型萃取 =========================
template<typename Ty>
struct ReflectVariableType
{
    using OriginalType = Ty;
    using PureType     = std::remove_cvref_t<Ty>;
};
template<typename Class, typename Ty>
struct ReflectVariableType<Ty Class::*>
{
    using OriginalType = Ty;
    using PureType     = std::remove_cvref_t<Ty>;
};

// ========================= FunctionTraits =========================
template<typename Ty> struct ReflectFunctionTraits;

template<typename Ret, typename... Args>
struct ReflectFunctionTraits<Ret(Args...)>
{
    using ReturnType = Ret;
    using ArgumentTypes = std::tuple<Args...>;
    using ClassType = void;
    static constexpr std::size_t Arity = sizeof...(Args);
    static constexpr bool IsMemberFunction = false;
    static constexpr bool IsConst = false;
    static constexpr bool IsFunction = true;
    static constexpr bool IsVariable = false;
};

template<typename Ret, typename Class, typename... Args>
struct ReflectFunctionTraits<Ret(Class::*)(Args...)>
{
    using ReturnType = Ret;
    using ArgumentTypes = std::tuple<Class *, Args...>;
    using ClassType = Class;
    static constexpr std::size_t Arity = sizeof...(Args);
    static constexpr bool IsMemberFunction = true;
    static constexpr bool IsConst = false;
    static constexpr bool IsFunction = true;
    static constexpr bool IsVariable = false;
};

template<typename Ret, typename Class, typename... Args>
struct ReflectFunctionTraits<Ret(Class::*)(Args...) const>
{
    using ReturnType = Ret;
    using ArgumentTypes = std::tuple<const Class *, Args...>;
    using ClassType = Class;
    static constexpr std::size_t Arity = sizeof...(Args);
    static constexpr bool IsMemberFunction = true;
    static constexpr bool IsConst = true;
    static constexpr bool IsFunction = true;
    static constexpr bool IsVariable = false;
};

// ========================= VariableTraits =========================
template<typename Ty>
struct ReflectVariableTraits
{
    using OriginalType = typename ReflectVariableType<Ty>::OriginalType;
    using PureType     = typename ReflectVariableType<Ty>::PureType;
    using ClassType    = void;
    using ReturnType   = void;
    using ArgumentTypes = std::tuple<>;
    static constexpr std::size_t Arity = 0;
    static constexpr bool IsMemberFunction = false;
    static constexpr bool IsConst = false;
    static constexpr bool IsFunction = false;
    static constexpr bool IsVariable = true;
    static constexpr bool IsMemberPointer = false;
    static constexpr bool IsPointer = std::is_pointer_v<Ty>;
    static constexpr bool IsReference = std::is_reference_v<Ty>;
};

template<typename Ty>
    requires std::is_member_object_pointer_v<Ty>
struct ReflectVariableTraits<Ty>
{
    using OriginalType = typename ReflectVariableType<Ty>::OriginalType;
    using PureType     = typename ReflectVariableType<Ty>::PureType;
    using ClassType    = ReflectMemberPointerClass_t<Ty>;
    using ReturnType   = void;
    using ArgumentTypes = std::tuple<>;
    static constexpr std::size_t Arity = 0;
    static constexpr bool IsMemberFunction = false;
    static constexpr bool IsConst = std::is_const_v<std::remove_reference_t<OriginalType>>;
    static constexpr bool IsFunction = false;
    static constexpr bool IsVariable = true;
    static constexpr bool IsMemberPointer = true;
    static constexpr bool IsPointer = false;
    static constexpr bool IsReference = false;
};

// ========================= BasicFieldTraits =========================
template<typename Ty>
struct ReflectBasicFieldTraits
{
private:
    static constexpr bool IsMemFn = std::is_member_function_pointer_v<Ty>;
    using Traits = std::conditional_t<IsMemFn, ReflectFunctionTraits<Ty>, ReflectVariableTraits<Ty>>;
public:
    using ReturnType = typename Traits::ReturnType;
    using ArgumentTypes = typename Traits::ArgumentTypes;
    using ClassType = typename Traits::ClassType;
    static constexpr std::size_t Arity = Traits::Arity;
    static constexpr bool IsMemberFunction = Traits::IsMemberFunction;
    static constexpr bool IsConst = Traits::IsConst;
    static constexpr bool IsFunction = Traits::IsFunction;
    static constexpr bool IsVariable = Traits::IsVariable;
};

// ========================= FieldTraits =========================
template<typename Ty>
struct ReflectFieldTraits : ReflectBasicFieldTraits<Ty>
{
    Ty Pointer;
    std::string_view Name;

    static constexpr std::string_view realName(std::string_view raw) noexcept
    {
        auto pos = raw.find_last_of(':');
        return (pos == std::string_view::npos) ? raw : raw.substr(pos + 1);
    }
    constexpr ReflectFieldTraits(Ty Ptr, std::string_view raw) noexcept
        : Pointer(Ptr), Name(realName(raw)) {}
};
template<typename Ty>
ReflectFieldTraits(Ty, std::string_view) -> ReflectFieldTraits<Ty>;

// ========================= 检测 Ty 是否有 reflect_* 静态成员 =========================
namespace detail
{
    template<typename Ty, typename = void>
    struct NameOf { static constexpr std::string_view Value = ""; };
    template<typename Ty>
    struct NameOf<Ty, std::void_t<decltype(Ty::reflect_name)>> { static constexpr std::string_view Value = Ty::reflect_name; };

    template<typename Ty, typename = void>
    struct BaseOf { using Type = ReflectNullBase; };
    template<typename Ty>
    struct BaseOf<Ty, std::void_t<typename Ty::reflect_base>> { using Type = typename Ty::reflect_base; };

    template<typename Ty, typename = void>
    struct VariablesOf { static constexpr auto Value = std::tuple{}; };
    template<typename Ty>
    struct VariablesOf<Ty, std::void_t<decltype(Ty::reflect_variables)>> { static constexpr auto Value = Ty::reflect_variables; };

    template<typename Ty, typename = void>
    struct FunctionsOf { static constexpr auto Value = std::tuple{}; };
    template<typename Ty>
    struct FunctionsOf<Ty, std::void_t<decltype(Ty::reflect_functions)>> { static constexpr auto Value = Ty::reflect_functions; };
}

// ========================= 通用 TypeInfo =========================
template<typename Ty>
struct TypeInfo
{
    static const inline char TypeId{};   // 每类型唯一标识（取地址作运行时 ID，不依赖 RTTI）
    static constexpr std::string_view Name = detail::NameOf<Ty>::Value;
    using Base = typename detail::BaseOf<Ty>::Type;
    static constexpr bool HasBase = !std::is_same_v<Base, ReflectNullBase>;
    static constexpr auto Functions = detail::FunctionsOf<Ty>::Value;
    static constexpr auto Variables = detail::VariablesOf<Ty>::Value;

    static constexpr const char *getClassName() { return Name.data(); }

    template<typename Callable>
    static void forEachMembers(Ty &Obj, Callable &&Fn)
    {
        if constexpr (HasBase) TypeInfo<Base>::forEachMembers(Obj, std::forward<Callable>(Fn));
        std::apply([&](auto &...Field) {
            (Fn(Field.Name.data(), Obj.*(Field.Pointer)), ...);
        }, Variables);
    }
    template<typename Callable>
    static void forEachMembers(const Ty &Obj, Callable &&Fn)
    {
        if constexpr (HasBase) TypeInfo<Base>::forEachMembers(Obj, std::forward<Callable>(Fn));
        std::apply([&](auto &...Field) {
            (Fn(Field.Name.data(), Obj.*(Field.Pointer)), ...);
        }, Variables);
    }
    template<typename Callable>
    static void forEachFunction(Callable &&Fn)
    {
        if constexpr (HasBase) TypeInfo<Base>::forEachFunction(std::forward<Callable>(Fn));
        std::apply([&](auto &...Field) { (Fn(Field), ...); }, Functions);
    }
};

template<typename Ty>
constexpr TypeInfo<Ty> StaticClass() { return TypeInfo<Ty>{}; }

// ========================= 类型检测（toAdt/fromAdt 用） =========================
namespace detail
{
    // 说明：std::false_type / std::true_type 的成员是 value（小写），本文件统一以 Value 暴露
    //       （与 NameOf / VariablesOf / FunctionsOf 保持一致），否则 ::Value 不是合法成员。
    template<typename Ty, typename = void>
    struct IsReflected : std::false_type { static constexpr bool Value = false; };
    template<typename Ty>
    struct IsReflected<Ty, std::void_t<decltype(Ty::reflect_variables)>> : std::true_type { static constexpr bool Value = true; };

    template<typename Ty> struct IsStdVector : std::false_type { static constexpr bool Value = false; };
    template<typename Ty, typename A> struct IsStdVector<std::vector<Ty, A>> : std::true_type { static constexpr bool Value = true; };

    template<typename Ty> struct IsStdOptional : std::false_type { static constexpr bool Value = false; };
    template<typename Ty> struct IsStdOptional<std::optional<Ty>> : std::true_type { static constexpr bool Value = true; };

    template<typename Ty> struct IsStringMap : std::false_type { static constexpr bool Value = false; };
    template<typename V, typename... R> struct IsStringMap<std::map<std::string, V, R...>> : std::true_type { static constexpr bool Value = true; };
    template<typename V, typename... R> struct IsStringMap<std::unordered_map<std::string, V, R...>> : std::true_type { static constexpr bool Value = true; };
}

// ========================= 业务类型 -> SerdeADT（反射驱动） =========================
template<typename Ty>
SerdeADT toAdt(const Ty &Obj)
{
    if constexpr (detail::IsReflected<Ty>::Value)
    {
        SerdeObject Members;
        TypeInfo<Ty>::forEachMembers(Obj, [&Members](const char *Name, const auto &Member) {
            Members.emplace_back(Name, toAdt(Member));
        });
        return SerdeADT{ std::move(Members) };
    }
    else if constexpr (detail::IsStdVector<Ty>::Value)
    {
        SerdeArray Arr;
        for (const auto &E : Obj) Arr.push_back(toAdt(E));
        return SerdeADT{ std::move(Arr) };
    }
    else if constexpr (detail::IsStdOptional<Ty>::Value)
    {
        return Obj ? toAdt(*Obj) : SerdeADT{ nullptr };
    }
    else if constexpr (detail::IsStringMap<Ty>::Value)
    {
        SerdeObject M;
        for (const auto &[K, Value] : Obj) M.emplace_back(K, toAdt(Value));
        return SerdeADT{ std::move(M) };
    }
    else
    {
        return SerdeADT{ Obj }; // 标量
    }
}

// ========================= SerdeADT -> 业务类型（反射驱动） =========================
template<typename Ty>
std::expected<Ty, std::string> fromAdt(const SerdeADT &Value)
{
    if constexpr (detail::IsReflected<Ty>::Value)
    {
        try
        {
            if (!Value.isObject()) return std::unexpected("expected object for reflected type");
            Ty Obj{};
            TypeInfo<Ty>::forEachMembers(Obj, [&Value](const char *Name, auto &Member) {
                if (const SerdeADT *F = Value.find(Name))
                {
                    auto R = fromAdt<std::decay_t<decltype(Member)>>(*F);
                    if (!R) throw std::runtime_error(R.error());
                    Member = std::move(*R);
                }
            });
            return Obj;
        }
        catch (const std::exception &E)
        {
            return std::unexpected(E.what());
        }
    }
    else if constexpr (detail::IsStdVector<Ty>::Value)
    {
        if (!Value.isArray()) return std::unexpected("expected array");
        Ty Vec;
        for (const auto &E : Value.asArray())
        {
            auto R = fromAdt<typename Ty::value_type>(E);
            if (!R) return std::unexpected(R.error());
            Vec.push_back(std::move(*R));
        }
        return Vec;
    }
    else if constexpr (detail::IsStdOptional<Ty>::Value)
    {
        if (Value.isNull()) return Ty{ std::nullopt };
        auto R = fromAdt<typename Ty::value_type>(Value);
        if (!R) return std::unexpected(R.error());
        return Ty{ std::move(*R) };
    }
    else if constexpr (detail::IsStringMap<Ty>::Value)
    {
        if (!Value.isObject()) return std::unexpected("expected object");
        Ty M;
        for (const auto &[K, E] : Value.asObject())
        {
            auto R = fromAdt<typename Ty::mapped_type>(E);
            if (!R) return std::unexpected(R.error());
            M.emplace(K, std::move(*R));
        }
        return M;
    }
    else
    {
        if constexpr (std::is_same_v<Ty, std::string>)
        {
            if (!Value.isString()) return std::unexpected("expected string");
            return Value.asString();
        }
        else if constexpr (std::is_same_v<Ty, bool>)
        {
            if (!Value.isBool()) return std::unexpected("expected bool");
            return Value.asBool();
        }
        else if constexpr (std::is_integral_v<Ty>)
        {
            if (Value.isInt()) return static_cast<Ty>(Value.asInt());
            if (Value.isUint()) return static_cast<Ty>(Value.asUint());
            if (Value.isDouble()) return static_cast<Ty>(Value.asDouble());
            return std::unexpected("expected integer");
        }
        else if constexpr (std::is_floating_point_v<Ty>)
        {
            if (Value.isDouble()) return static_cast<Ty>(Value.asDouble());
            if (Value.isInt()) return static_cast<Ty>(Value.asInt());
            if (Value.isUint()) return static_cast<Ty>(Value.asUint());
            return std::unexpected("expected number");
        }
        else
        {
            return std::unexpected("fromAdt: unsupported type");
        }
    }
}

// 抛异常版（供运行时 invoke 内部使用）
template<typename Ty>
Ty valueCast(const SerdeADT &Value)
{
    auto R = fromAdt<Ty>(Value);
    if (!R) throw std::runtime_error(R.error());
    return std::move(*R);
}

class ReflectObject
{
public:
    ReflectObject() noexcept = default;

    template<typename Ty>
        requires (!std::is_same_v<std::decay_t<Ty>, ReflectObject>)
    ReflectObject(Ty &&Value)
    {
        using U = std::decay_t<Ty>;
        Ptr = new U(std::forward<Ty>(Value));
        Id = &TypeInfo<U>::TypeId;
        Vtable = &VTable<U>::get();
    }

    ReflectObject(const ReflectObject &Other)
    {
        if (Other.Ptr) { Ptr = Other.Vtable->clone(Other.Ptr); Id = Other.Id; Vtable = Other.Vtable; }
    }
    ReflectObject(ReflectObject &&Other) noexcept
    {
        Ptr          = Other.Ptr; 
        Id           = Other.Id; 
        Vtable       = Other.Vtable;
        Other.Ptr    = nullptr; 
        Other.Id     = nullptr; 
        Other.Vtable = nullptr;
    }
    ReflectObject &operator=(const ReflectObject &Other)
    {
        if (this != &Other)
        {
            reset();
            if (Other.Ptr) 
            { 
                Ptr    = Other.Vtable->clone(Other.Ptr); 
                Id     = Other.Id; 
                Vtable = Other.Vtable; 
            }
        }
        return *this;
    }
    ReflectObject &operator=(ReflectObject &&Other) noexcept
    {
        if (this != &Other) 
        { 
            reset(); 
            Ptr          = Other.Ptr; 
            Id           = Other.Id; 
            Vtable       = Other.Vtable; 
            Other.Ptr    = nullptr; 
            Other.Id     = nullptr; 
            Other.Vtable = nullptr; 
        }
        return *this;
    }
    ~ReflectObject() { reset(); }

    [[nodiscard]] bool hasValue() const noexcept { return Ptr != nullptr; }

    template<typename Ty>
    [[nodiscard]] bool is() const noexcept
    {
        return Ptr != nullptr && Id == &TypeInfo<std::decay_t<Ty>>::TypeId;
    }

    template<typename Ty> Ty &cast()
    {
        using U = std::decay_t<Ty>;
        if (!is<U>()) 
        {
            throw std::bad_cast();
        }
        return *static_cast<U *>(Ptr);
    }
    template<typename Ty> const Ty &cast() const
    {
        using U = std::decay_t<Ty>;
        if (!is<U>()) 
        {
            throw std::bad_cast();
        }
        return *static_cast<const U *>(Ptr);
    }

private:
    struct VTableBase { void *(*clone)(const void *); void (*destroy)(void *); };
    template<typename Ty>
    struct VTable
    {
        static void *clone(const void *p) { return new Ty(*static_cast<const Ty *>(p)); }
        static void destroy(void *p) { delete static_cast<Ty *>(p); }
        static const VTableBase &get() 
        { 
            static const VTableBase Vt{ &clone, &destroy }; 
            return Vt;
        }
    };
    void reset() noexcept
    {
        if (Ptr) 
        {
            Vtable->destroy(Ptr);
        }
        Ptr    = nullptr; 
        Id     = nullptr; 
        Vtable = nullptr;
    }
    void *Ptr { nullptr };
    const void *Id { nullptr };
    const VTableBase *Vtable { nullptr };
};

// ========================= 运行时反射：参数元组（去掉隐式 this） =========================
template<typename Tuple> struct ReflectArgsTuple;
template<typename First, typename... Rest>
struct ReflectArgsTuple<std::tuple<First, Rest...>> { using type = std::tuple<Rest...>; };

template<typename Class, typename MemFn, typename Ret, typename... Args>
SerdeADT invokeMemberImpl(Class &self, MemFn Fn, const SerdeADT &args, std::tuple<Args...> *)
{
    if (!args.isArray()) throw std::invalid_argument("invoke: arguments must be an array");
    const auto &Arr = args.asArray();
    if (Arr.size() != sizeof...(Args)) throw std::invalid_argument("invoke: argument count mismatch");
    return [&]<std::size_t... I>(std::index_sequence<I...>) -> SerdeADT {
        if constexpr (std::is_void_v<Ret>)
        {
            (self.*Fn)(valueCast<std::remove_cvref_t<Args>>(Arr[I])...);
            return SerdeADT{ nullptr };
        }
        else
        {
            return toAdt((self.*Fn)(valueCast<std::remove_cvref_t<Args>>(Arr[I])...));
        }
    }(std::index_sequence_for<Args...>{});
}

template<typename Class, typename MemFn>
SerdeADT invokeMember(Class &self, MemFn Fn, const SerdeADT &args)
{
    using Traits       = ReflectFunctionTraits<MemFn>;
    using ExplicitArgs = typename ReflectArgsTuple<typename Traits::ArgumentTypes>::type;
    return invokeMemberImpl<Class, MemFn, typename Traits::ReturnType>(self, Fn, args, static_cast<ExplicitArgs *>(nullptr));
}

// ========================= 运行时反射：全局注册表 =========================
class ReflectRegistry
{
public:
    using Factory = std::function<ReflectObject(const SerdeADT &)>;
    using Invoker = std::function<SerdeADT(ReflectObject &, const SerdeADT &)>;
    using Getter  = std::function<SerdeADT(ReflectObject &)>;
    using Setter  = std::function<void(ReflectObject &, const SerdeADT &)>;

    static ReflectRegistry &self()
    {
        static ReflectRegistry Instance;
        return Instance;
    }

    void registerEntry(std::string_view Name, Factory factory,
                       std::unordered_map<std::string, Invoker> invokers,
                       std::unordered_map<std::string, Getter> Getters,
                       std::unordered_map<std::string, Setter> Setters)
    {
        Types.emplace(std::string(Name),
                       Entry{ std::move(factory), std::move(invokers), std::move(Getters), std::move(Setters) });
    }

    [[nodiscard]] bool has(std::string_view Name) const
    {
        return Types.find(std::string(Name)) != Types.end();
    }

    // 通过字符串构造对象
    ReflectObject create(std::string_view Name) const
    {
        return create(Name, SerdeADT{ nullptr });
    }
    ReflectObject create(std::string_view Name, const SerdeADT &args) const
    {
        auto it = Types.find(std::string(Name));
        if (it == Types.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(Name) + "'");
        }
        return it->second.Factory(args);
    }

    // 通过字符串调用成员方法
    SerdeADT invoke(std::string_view type, std::string_view Fn, ReflectObject &Obj, const SerdeADT &args) const
    {
        auto it = Types.find(std::string(type));
        if (it == Types.end())
        {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type) + "'");
        }
        auto fit = it->second.Invokers.find(std::string(Fn));
        if (fit == it->second.Invokers.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown function '" + std::string(Fn) + "'");
        }
        return fit->second(Obj, args);
    }

    // 通过字符串访问成员, 读
    SerdeADT get(std::string_view type, std::string_view Member, ReflectObject &Obj) const
    {
        auto it = Types.find(std::string(type));
        if (it == Types.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type) + "'");
        }
        auto git = it->second.Getters.find(std::string(Member));
        if (git == it->second.Getters.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown Member '" + std::string(Member) + "'");
        }
        return git->second(Obj);
    }

    // 通过字符串访问成员, 写
    void set(std::string_view type, std::string_view Member, ReflectObject &Obj, const SerdeADT &Value) const
    {
        auto it = Types.find(std::string(type));
        if (it == Types.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type) + "'");
        }
        auto sit = it->second.Setters.find(std::string(Member));
        if (sit == it->second.Setters.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown Member '" + std::string(Member) + "'");
        }
        sit->second(Obj, Value);
    }

private:
    struct Entry
    {
        Factory Factory;
        std::unordered_map<std::string, Invoker> Invokers;
        std::unordered_map<std::string, Getter> Getters;
        std::unordered_map<std::string, Setter> Setters;
    };
    std::unordered_map<std::string, Entry> Types;
};

// ========================= 由 TypeInfo<Ty> 生成工厂 / 调用表 / 成员表 =========================
template<typename Ty>
ReflectObject factory(const SerdeADT &args)
{
    if (args.isNull()) return ReflectObject{ Ty{} };
    auto R = fromAdt<Ty>(args);
    if (!R)
    {
        throw std::runtime_error(R.error());
    }
    return ReflectObject{ std::move(*R) };
}

template<typename Ty>
std::unordered_map<std::string, ReflectRegistry::Invoker> buildInvokers()
{
    std::unordered_map<std::string, ReflectRegistry::Invoker> Invokers;
    TypeInfo<Ty>::forEachFunction([&Invokers](const auto &Field) {
        Invokers[std::string(Field.Name)] =
            [Ptr = Field.Pointer](ReflectObject &Obj, const SerdeADT &args) -> SerdeADT {
                return invokeMember(Obj.cast<Ty>(), Ptr, args);
            };
    });
    return Invokers;
}

template<typename Ty, typename FieldType>
void addMemberField(std::unordered_map<std::string, ReflectRegistry::Getter> &Getters,
                    std::unordered_map<std::string, ReflectRegistry::Setter> &Setters,
                    const FieldType &Field)
{
    auto Ptr = Field.Pointer;
    Getters[std::string(Field.Name)] = [Ptr](ReflectObject &Obj) -> SerdeADT {
        return toAdt(Obj.cast<Ty>().*Ptr);
    };
    Setters[std::string(Field.Name)] = [Ptr](ReflectObject &Obj, const SerdeADT &Value) {
        using FieldT = std::decay_t<decltype(Obj.cast<Ty>().*Ptr)>;
        auto R = fromAdt<FieldT>(Value);
        if (!R) throw std::runtime_error(R.error());
        Obj.cast<Ty>().*Ptr = std::move(*R);
    };
}

template<typename Ty>
void buildMembersInto(std::unordered_map<std::string, ReflectRegistry::Getter> &Getters,
                      std::unordered_map<std::string, ReflectRegistry::Setter> &Setters)
{
    if constexpr (TypeInfo<Ty>::HasBase)
        buildMembersInto<typename TypeInfo<Ty>::Base>(Getters, Setters);
    std::apply([&](auto &...Field) {
        ((addMemberField<Ty>(Getters, Setters, Field)), ...);
    }, TypeInfo<Ty>::Variables);
}

template<typename Ty>
void registerReflect()
{
    auto Invokers = buildInvokers<Ty>();
    std::unordered_map<std::string, ReflectRegistry::Getter> Getters;
    std::unordered_map<std::string, ReflectRegistry::Setter> Setters;
    buildMembersInto<Ty>(Getters, Setters);
    ReflectRegistry::self().registerEntry(TypeInfo<Ty>::Name, &factory<Ty>,
                                          std::move(Invokers), std::move(Getters), std::move(Setters));
}

} // namespace core

// 全局别名：让用户写 REFLECT_CLASS(Person, ReflectNullBase) 时无需 core:: 前缀
using ReflectNullBase = core::ReflectNullBase;

// ========================= 注册宏 =========================
#define REFLECT_FIRST_SELECT(first, ...) first
#define REFLECT_FIRST(...) REFLECT_FIRST_SELECT(__VA_ARGS__ __VA_OPT__(,) ReflectNullBase)

#define PP_CAT(a, b) PP_CAT_I(a, b)
#define PP_CAT_I(a, b) a##b
#define PP_NARG(...) PP_NARG_I(__VA_ARGS__, 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)
#define PP_NARG_I(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,N,...) N

#define PP_FOR_EACH(F, ...) PP_CAT(PP_FOR_EACH_, PP_NARG(__VA_ARGS__))(F, __VA_ARGS__)
#define PP_FOR_EACH_1(F,a) F(a)
#define PP_FOR_EACH_2(F,a,b) F(a) F(b)
#define PP_FOR_EACH_3(F,a,b,c) F(a) F(b) F(c)
#define PP_FOR_EACH_4(F,a,b,c,d) F(a) F(b) F(c) F(d)
#define PP_FOR_EACH_5(F,a,b,c,d,E) F(a) F(b) F(c) F(d) F(E)
#define PP_FOR_EACH_6(F,a,b,c,d,E,f) F(a) F(b) F(c) F(d) F(E) F(f)
#define PP_FOR_EACH_7(F,a,b,c,d,E,f,g) F(a) F(b) F(c) F(d) F(E) F(f) F(g)
#define PP_FOR_EACH_8(F,a,b,c,d,E,f,g,h) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h)
#define PP_FOR_EACH_9(F,a,b,c,d,E,f,g,h,i) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i)
#define PP_FOR_EACH_10(F,a,b,c,d,E,f,g,h,i,j) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j)
#define PP_FOR_EACH_11(F,a,b,c,d,E,f,g,h,i,j,K) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K)
#define PP_FOR_EACH_12(F,a,b,c,d,E,f,g,h,i,j,K,l) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K) F(l)
#define PP_FOR_EACH_13(F,a,b,c,d,E,f,g,h,i,j,K,l,M) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K) F(l) F(M)
#define PP_FOR_EACH_14(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K) F(l) F(M) F(n)
#define PP_FOR_EACH_15(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n,o) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K) F(l) F(M) F(n) F(o)
#define PP_FOR_EACH_16(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n,o,p) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K) F(l) F(M) F(n) F(o) F(p)

#define PP_MAP(F, ...) PP_CAT(PP_MAP_, PP_NARG(__VA_ARGS__))(F, __VA_ARGS__)
#define PP_MAP_1(F,a) F(a)
#define PP_MAP_2(F,a,b) F(a), F(b)
#define PP_MAP_3(F,a,b,c) F(a), F(b), F(c)
#define PP_MAP_4(F,a,b,c,d) F(a), F(b), F(c), F(d)
#define PP_MAP_5(F,a,b,c,d,E) F(a), F(b), F(c), F(d), F(E)
#define PP_MAP_6(F,a,b,c,d,E,f) F(a), F(b), F(c), F(d), F(E), F(f)
#define PP_MAP_7(F,a,b,c,d,E,f,g) F(a), F(b), F(c), F(d), F(E), F(f), F(g)
#define PP_MAP_8(F,a,b,c,d,E,f,g,h) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h)
#define PP_MAP_9(F,a,b,c,d,E,f,g,h,i) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i)
#define PP_MAP_10(F,a,b,c,d,E,f,g,h,i,j) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j)
#define PP_MAP_11(F,a,b,c,d,E,f,g,h,i,j,K) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K)
#define PP_MAP_12(F,a,b,c,d,E,f,g,h,i,j,K,l) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K), F(l)
#define PP_MAP_13(F,a,b,c,d,E,f,g,h,i,j,K,l,M) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K), F(l), F(M)
#define PP_MAP_14(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K), F(l), F(M), F(n)
#define PP_MAP_15(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n,o) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K), F(l), F(M), F(n), F(o)
#define PP_MAP_16(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n,o,p) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K), F(l), F(M), F(n), F(o), F(p)

// 类元信息 + staticClass；须写在类内、先于 REFLECT_FIELDS / REFLECT_FUNCTIONS
#define REFLECT_CLASS(Type, ...)                              \
    static constexpr std::string_view reflect_name = #Type;   \
    using reflect_self = Type;                                \
    using reflect_base = REFLECT_FIRST(__VA_ARGS__);          \
    static auto StaticClass() { return core::TypeInfo<Type>{}; }

#define REFLECT_FIELD_DECL(P)  REFLECT_FIELD_DECL_I P
#define REFLECT_FIELD_DECL_I(Ty, N) Ty N;
#define REFLECT_FIELD_ENTRY(P) REFLECT_FIELD_ENTRY_I P
#define REFLECT_FIELD_ENTRY_I(Ty, N) core::ReflectFieldTraits{ &reflect_self::N, #N }
#define REFLECT_FIELDS(...)                                   \
    PP_FOR_EACH(REFLECT_FIELD_DECL, __VA_ARGS__)              \
    static constexpr auto reflect_variables =                 \
        std::make_tuple(PP_MAP(REFLECT_FIELD_ENTRY, __VA_ARGS__))

#define REFLECT_FN_ENTRY(N) core::ReflectFieldTraits{ &reflect_self::N, #N }
#define REFLECT_FUNCTIONS(...)                                \
    static constexpr auto reflect_functions =                 \
        std::make_tuple(__VA_OPT__(PP_MAP(REFLECT_FN_ENTRY, __VA_ARGS__)))

#endif // REFLECT_REFLECT_H
