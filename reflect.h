//
// Reflect.h — 反射（与序列化完全解耦）
//
// 职责分两块，均与 Serde 无关：
//   1. 编译期反射：ReflectTypeInfo / 字段函数元信息 + toAdt/fromAdt（业务类型 <-> SerdeADT 的桥）
//   2. 运行时反射：ReflectRegistry —— 通过字符串构造对象 / 访问成员 / 调用成员方法
//
// 业务类型只需用宏注册成员（REFLECT_CLASS / REFLECT_FIELDS / REFLECT_FUNCTIONS），
// 即可获得 toAdt/fromAdt 与字符串式运行时访问能力。
//

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
    constexpr ReflectFieldTraits(Ty ptr, std::string_view raw) noexcept
        : Pointer(ptr), Name(realName(raw)) {}
};
template<typename Ty>
ReflectFieldTraits(Ty, std::string_view) -> ReflectFieldTraits<Ty>;

// ========================= 检测 T 是否有 reflect_* 静态成员 =========================
namespace detail
{
    template<typename T, typename = void>
    struct NameOf { static constexpr std::string_view Value = ""; };
    template<typename T>
    struct NameOf<T, std::void_t<decltype(T::reflect_name)>> { static constexpr std::string_view Value = T::reflect_name; };

    template<typename T, typename = void>
    struct BaseOf { using Type = ReflectNullBase; };
    template<typename T>
    struct BaseOf<T, std::void_t<typename T::reflect_base>> { using Type = typename T::reflect_base; };

    template<typename T, typename = void>
    struct VariablesOf { static constexpr auto Value = std::tuple{}; };
    template<typename T>
    struct VariablesOf<T, std::void_t<decltype(T::reflect_variables)>> { static constexpr auto Value = T::reflect_variables; };

    template<typename T, typename = void>
    struct FunctionsOf { static constexpr auto Value = std::tuple{}; };
    template<typename T>
    struct FunctionsOf<T, std::void_t<decltype(T::reflect_functions)>> { static constexpr auto Value = T::reflect_functions; };
}

// ========================= 通用 TypeInfo =========================
template<typename T>
struct ReflectTypeInfo
{
    static const inline char TypeId{};   // 每类型唯一标识（取地址作运行时 ID，不依赖 RTTI）
    static constexpr std::string_view Name = detail::NameOf<T>::Value;
    using Base = typename detail::BaseOf<T>::Type;
    static constexpr bool HasBase = !std::is_same_v<Base, ReflectNullBase>;
    static constexpr auto Functions = detail::FunctionsOf<T>::Value;
    static constexpr auto Variables = detail::VariablesOf<T>::Value;

    static constexpr const char *getClassName() { return Name.data(); }

    template<typename Fn>
    static void forEachMembers(T &obj, Fn &&fn)
    {
        if constexpr (HasBase) ReflectTypeInfo<Base>::forEachMembers(obj, std::forward<Fn>(fn));
        std::apply([&](auto &...field) {
            (fn(field.Name.data(), obj.*(field.Pointer)), ...);
        }, Variables);
    }
    template<typename Fn>
    static void forEachMembers(const T &obj, Fn &&fn)
    {
        if constexpr (HasBase) ReflectTypeInfo<Base>::forEachMembers(obj, std::forward<Fn>(fn));
        std::apply([&](auto &...field) {
            (fn(field.Name.data(), obj.*(field.Pointer)), ...);
        }, Variables);
    }
    template<typename Fn>
    static void forEachFunction(Fn &&fn)
    {
        if constexpr (HasBase) ReflectTypeInfo<Base>::forEachFunction(std::forward<Fn>(fn));
        std::apply([&](auto &...field) { (fn(field), ...); }, Functions);
    }
};

template<typename T>
constexpr ReflectTypeInfo<T> staticClass() { return ReflectTypeInfo<T>{}; }

// ========================= 类型检测（toAdt/fromAdt 用） =========================
namespace detail
{
    template<typename T, typename = void>
    struct IsReflected : std::false_type {};
    template<typename T>
    struct IsReflected<T, std::void_t<decltype(T::reflect_variables)>> : std::true_type {};

    template<typename T> struct IsStdVector : std::false_type {};
    template<typename T, typename A> struct IsStdVector<std::vector<T, A>> : std::true_type {};

    template<typename T> struct IsStdOptional : std::false_type {};
    template<typename T> struct IsStdOptional<std::optional<T>> : std::true_type {};

    template<typename T> struct IsStringMap : std::false_type {};
    template<typename V, typename... R> struct IsStringMap<std::map<std::string, V, R...>> : std::true_type {};
    template<typename V, typename... R> struct IsStringMap<std::unordered_map<std::string, V, R...>> : std::true_type {};
}

// ========================= 业务类型 -> SerdeADT（反射驱动） =========================
template<typename T>
SerdeADT toAdt(const T &obj)
{
    if constexpr (detail::IsReflected<T>::value)
    {
        SerdeObject members;
        ReflectTypeInfo<T>::forEachMembers(obj, [&members](const char *name, const auto &member) {
            members.emplace_back(name, toAdt(member));
        });
        return SerdeADT{ std::move(members) };
    }
    else if constexpr (detail::IsStdVector<T>::value)
    {
        SerdeArray arr;
        for (const auto &e : obj) arr.push_back(toAdt(e));
        return SerdeADT{ std::move(arr) };
    }
    else if constexpr (detail::IsStdOptional<T>::value)
    {
        return obj ? toAdt(*obj) : SerdeADT{ nullptr };
    }
    else if constexpr (detail::IsStringMap<T>::value)
    {
        SerdeObject m;
        for (const auto &[k, v] : obj) m.emplace_back(k, toAdt(v));
        return SerdeADT{ std::move(m) };
    }
    else
    {
        return SerdeADT{ obj }; // 标量
    }
}

// ========================= SerdeADT -> 业务类型（反射驱动） =========================
template<typename T>
std::expected<T, std::string> fromAdt(const SerdeADT &v)
{
    if constexpr (detail::IsReflected<T>::value)
    {
        try
        {
            if (!v.isObject()) return std::unexpected("expected object for reflected type");
            T obj{};
            ReflectTypeInfo<T>::forEachMembers(obj, [&v](const char *name, auto &member) {
                if (const SerdeADT *f = v.find(name))
                {
                    auto r = fromAdt<std::decay_t<decltype(member)>>(*f);
                    if (!r) throw std::runtime_error(r.error());
                    member = std::move(*r);
                }
            });
            return obj;
        }
        catch (const std::exception &e)
        {
            return std::unexpected(e.what());
        }
    }
    else if constexpr (detail::IsStdVector<T>::value)
    {
        if (!v.isArray()) return std::unexpected("expected array");
        T vec;
        for (const auto &e : v.asArray())
        {
            auto r = fromAdt<typename T::value_type>(e);
            if (!r) return std::unexpected(r.error());
            vec.push_back(std::move(*r));
        }
        return vec;
    }
    else if constexpr (detail::IsStdOptional<T>::value)
    {
        if (v.isNull()) return T{ std::nullopt };
        auto r = fromAdt<typename T::value_type>(v);
        if (!r) return std::unexpected(r.error());
        return T{ std::move(*r) };
    }
    else if constexpr (detail::IsStringMap<T>::value)
    {
        if (!v.isObject()) return std::unexpected("expected object");
        T m;
        for (const auto &[k, e] : v.asObject())
        {
            auto r = fromAdt<typename T::mapped_type>(e);
            if (!r) return std::unexpected(r.error());
            m.emplace(k, std::move(*r));
        }
        return m;
    }
    else
    {
        if constexpr (std::is_same_v<T, std::string>)
        {
            if (!v.isString()) return std::unexpected("expected string");
            return v.asString();
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            if (!v.isBool()) return std::unexpected("expected bool");
            return v.asBool();
        }
        else if constexpr (std::is_integral_v<T>)
        {
            if (v.isInt()) return static_cast<T>(v.asInt());
            if (v.isUint()) return static_cast<T>(v.asUint());
            if (v.isDouble()) return static_cast<T>(v.asDouble());
            return std::unexpected("expected integer");
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            if (v.isDouble()) return static_cast<T>(v.asDouble());
            if (v.isInt()) return static_cast<T>(v.asInt());
            if (v.isUint()) return static_cast<T>(v.asUint());
            return std::unexpected("expected number");
        }
        else
        {
            return std::unexpected("fromAdt: unsupported type");
        }
    }
}

// 抛异常版（供运行时 invoke 内部使用）
template<typename T>
T valueCast(const SerdeADT &v)
{
    auto r = fromAdt<T>(v);
    if (!r) throw std::runtime_error(r.error());
    return std::move(*r);
}

// ========================= 运行时反射：类型擦除的活对象 =========================
class ReflectObject
{
public:
    ReflectObject() noexcept = default;

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, ReflectObject>)
    ReflectObject(T &&value)
    {
        using U = std::decay_t<T>;
        Ptr_ = new U(std::forward<T>(value));
        Id_ = &ReflectTypeInfo<U>::TypeId;
        Vtable_ = &VTable<U>::get();
    }

    ReflectObject(const ReflectObject &other)
    {
        if (other.Ptr_) { Ptr_ = other.Vtable_->clone(other.Ptr_); Id_ = other.Id_; Vtable_ = other.Vtable_; }
    }
    ReflectObject(ReflectObject &&other) noexcept
    {
        Ptr_ = other.Ptr_; Id_ = other.Id_; Vtable_ = other.Vtable_;
        other.Ptr_ = nullptr; other.Id_ = nullptr; other.Vtable_ = nullptr;
    }
    ReflectObject &operator=(const ReflectObject &other)
    {
        if (this != &other)
        {
            reset();
            if (other.Ptr_) { Ptr_ = other.Vtable_->clone(other.Ptr_); Id_ = other.Id_; Vtable_ = other.Vtable_; }
        }
        return *this;
    }
    ReflectObject &operator=(ReflectObject &&other) noexcept
    {
        if (this != &other) { reset(); Ptr_ = other.Ptr_; Id_ = other.Id_; Vtable_ = other.Vtable_; other.Ptr_ = nullptr; other.Id_ = nullptr; other.Vtable_ = nullptr; }
        return *this;
    }
    ~ReflectObject() { reset(); }

    [[nodiscard]] bool hasValue() const noexcept { return Ptr_ != nullptr; }

    template<typename T>
    [[nodiscard]] bool is() const noexcept
    {
        return Ptr_ != nullptr && Id_ == &ReflectTypeInfo<std::decay_t<T>>::TypeId;
    }

    template<typename T> T &cast()
    {
        using U = std::decay_t<T>;
        if (!is<U>()) throw std::bad_cast();
        return *static_cast<U *>(Ptr_);
    }
    template<typename T> const T &cast() const
    {
        using U = std::decay_t<T>;
        if (!is<U>()) throw std::bad_cast();
        return *static_cast<const U *>(Ptr_);
    }

private:
    struct VTableBase { void *(*clone)(const void *); void (*destroy)(void *); };
    template<typename T>
    struct VTable
    {
        static void *clone(const void *p) { return new T(*static_cast<const T *>(p)); }
        static void destroy(void *p) { delete static_cast<T *>(p); }
        static const VTableBase &get() { static const VTableBase vt{ &clone, &destroy }; return vt; }
    };
    void reset() noexcept
    {
        if (Ptr_) Vtable_->destroy(Ptr_);
        Ptr_ = nullptr; Id_ = nullptr; Vtable_ = nullptr;
    }
    void *Ptr_ = nullptr;
    const void *Id_ = nullptr;
    const VTableBase *Vtable_ = nullptr;
};

// ========================= 运行时反射：参数元组（去掉隐式 this） =========================
template<typename Tuple> struct ReflectArgsTuple;
template<typename First, typename... Rest>
struct ReflectArgsTuple<std::tuple<First, Rest...>> { using type = std::tuple<Rest...>; };

template<typename Class, typename MemFn, typename Ret, typename... Args>
SerdeADT invokeMemberImpl(Class &self, MemFn fn, const SerdeADT &args, std::tuple<Args...> *)
{
    if (!args.isArray()) throw std::invalid_argument("invoke: arguments must be an array");
    const auto &arr = args.asArray();
    if (arr.size() != sizeof...(Args)) throw std::invalid_argument("invoke: argument count mismatch");
    return [&]<std::size_t... I>(std::index_sequence<I...>) -> SerdeADT {
        if constexpr (std::is_void_v<Ret>)
        {
            (self.*fn)(valueCast<std::remove_cvref_t<Args>>(arr[I])...);
            return SerdeADT{ nullptr };
        }
        else
        {
            return toAdt((self.*fn)(valueCast<std::remove_cvref_t<Args>>(arr[I])...));
        }
    }(std::index_sequence_for<Args...>{});
}

template<typename Class, typename MemFn>
SerdeADT invokeMember(Class &self, MemFn fn, const SerdeADT &args)
{
    using Traits = ReflectFunctionTraits<MemFn>;
    using ExplicitArgs = typename ReflectArgsTuple<typename Traits::ArgumentTypes>::type;
    return invokeMemberImpl<Class, MemFn, typename Traits::ReturnType>(self, fn, args, static_cast<ExplicitArgs *>(nullptr));
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

    void registerEntry(std::string_view name, Factory factory,
                       std::unordered_map<std::string, Invoker> invokers,
                       std::unordered_map<std::string, Getter> getters,
                       std::unordered_map<std::string, Setter> setters)
    {
        types_.emplace(std::string(name),
                       Entry{ std::move(factory), std::move(invokers), std::move(getters), std::move(setters) });
    }

    [[nodiscard]] bool has(std::string_view name) const
    {
        return types_.find(std::string(name)) != types_.end();
    }

    // 通过字符串构造对象
    ReflectObject create(std::string_view name) const
    {
        return create(name, SerdeADT{ nullptr });
    }
    ReflectObject create(std::string_view name, const SerdeADT &args) const
    {
        auto it = types_.find(std::string(name));
        if (it == types_.end()) throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(name) + "'");
        return it->second.Factory(args);
    }

    // 通过字符串调用成员方法
    SerdeADT invoke(std::string_view type, std::string_view fn, ReflectObject &obj, const SerdeADT &args) const
    {
        auto it = types_.find(std::string(type));
        if (it == types_.end()) throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type) + "'");
        auto fit = it->second.Invokers.find(std::string(fn));
        if (fit == it->second.Invokers.end()) throw std::invalid_argument("ReflectRegistry: unknown function '" + std::string(fn) + "'");
        return fit->second(obj, args);
    }

    // 通过字符串访问成员（读）
    SerdeADT get(std::string_view type, std::string_view member, ReflectObject &obj) const
    {
        auto it = types_.find(std::string(type));
        if (it == types_.end()) throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type) + "'");
        auto git = it->second.Getters.find(std::string(member));
        if (git == it->second.Getters.end()) throw std::invalid_argument("ReflectRegistry: unknown member '" + std::string(member) + "'");
        return git->second(obj);
    }

    // 通过字符串访问成员（写）
    void set(std::string_view type, std::string_view member, ReflectObject &obj, const SerdeADT &value) const
    {
        auto it = types_.find(std::string(type));
        if (it == types_.end()) throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type) + "'");
        auto sit = it->second.Setters.find(std::string(member));
        if (sit == it->second.Setters.end()) throw std::invalid_argument("ReflectRegistry: unknown member '" + std::string(member) + "'");
        sit->second(obj, value);
    }

private:
    struct Entry
    {
        Factory Factory;
        std::unordered_map<std::string, Invoker> Invokers;
        std::unordered_map<std::string, Getter> Getters;
        std::unordered_map<std::string, Setter> Setters;
    };
    std::unordered_map<std::string, Entry> types_;
};

// ========================= 由 ReflectTypeInfo<T> 生成工厂 / 调用表 / 成员表 =========================
template<typename T>
ReflectObject factory(const SerdeADT &args)
{
    if (args.isNull()) return ReflectObject{ T{} };
    auto r = fromAdt<T>(args);
    if (!r) throw std::runtime_error(r.error());
    return ReflectObject{ std::move(*r) };
}

template<typename T>
std::unordered_map<std::string, ReflectRegistry::Invoker> buildInvokers()
{
    std::unordered_map<std::string, ReflectRegistry::Invoker> invokers;
    ReflectTypeInfo<T>::forEachFunction([&invokers](const auto &field) {
        invokers[std::string(field.Name)] =
            [ptr = field.Pointer](ReflectObject &obj, const SerdeADT &args) -> SerdeADT {
                return invokeMember(obj.cast<T>(), ptr, args);
            };
    });
    return invokers;
}

template<typename T, typename Field>
void addMemberField(std::unordered_map<std::string, ReflectRegistry::Getter> &getters,
                    std::unordered_map<std::string, ReflectRegistry::Setter> &setters,
                    const Field &field)
{
    auto ptr = field.Pointer;
    getters[std::string(field.Name)] = [ptr](ReflectObject &obj) -> SerdeADT {
        return toAdt(obj.cast<T>().*ptr);
    };
    setters[std::string(field.Name)] = [ptr](ReflectObject &obj, const SerdeADT &v) {
        using FieldT = std::decay_t<decltype(obj.cast<T>().*ptr)>;
        auto r = fromAdt<FieldT>(v);
        if (!r) throw std::runtime_error(r.error());
        obj.cast<T>().*ptr = std::move(*r);
    };
}

template<typename T>
void buildMembersInto(std::unordered_map<std::string, ReflectRegistry::Getter> &getters,
                      std::unordered_map<std::string, ReflectRegistry::Setter> &setters)
{
    if constexpr (ReflectTypeInfo<T>::HasBase)
        buildMembersInto<typename ReflectTypeInfo<T>::Base>(getters, setters);
    std::apply([&](auto &...field) {
        ((addMemberField<T>(getters, setters, field)), ...);
    }, ReflectTypeInfo<T>::Variables);
}

template<typename T>
void registerReflect()
{
    auto invokers = buildInvokers<T>();
    std::unordered_map<std::string, ReflectRegistry::Getter> getters;
    std::unordered_map<std::string, ReflectRegistry::Setter> setters;
    buildMembersInto<T>(getters, setters);
    ReflectRegistry::self().registerEntry(ReflectTypeInfo<T>::Name, &factory<T>,
                                          std::move(invokers), std::move(getters), std::move(setters));
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
#define PP_FOR_EACH_5(F,a,b,c,d,e) F(a) F(b) F(c) F(d) F(e)
#define PP_FOR_EACH_6(F,a,b,c,d,e,f) F(a) F(b) F(c) F(d) F(e) F(f)
#define PP_FOR_EACH_7(F,a,b,c,d,e,f,g) F(a) F(b) F(c) F(d) F(e) F(f) F(g)
#define PP_FOR_EACH_8(F,a,b,c,d,e,f,g,h) F(a) F(b) F(c) F(d) F(e) F(f) F(g) F(h)
#define PP_FOR_EACH_9(F,a,b,c,d,e,f,g,h,i) F(a) F(b) F(c) F(d) F(e) F(f) F(g) F(h) F(i)
#define PP_FOR_EACH_10(F,a,b,c,d,e,f,g,h,i,j) F(a) F(b) F(c) F(d) F(e) F(f) F(g) F(h) F(i) F(j)
#define PP_FOR_EACH_11(F,a,b,c,d,e,f,g,h,i,j,k) F(a) F(b) F(c) F(d) F(e) F(f) F(g) F(h) F(i) F(j) F(k)
#define PP_FOR_EACH_12(F,a,b,c,d,e,f,g,h,i,j,k,l) F(a) F(b) F(c) F(d) F(e) F(f) F(g) F(h) F(i) F(j) F(k) F(l)
#define PP_FOR_EACH_13(F,a,b,c,d,e,f,g,h,i,j,k,l,m) F(a) F(b) F(c) F(d) F(e) F(f) F(g) F(h) F(i) F(j) F(k) F(l) F(m)
#define PP_FOR_EACH_14(F,a,b,c,d,e,f,g,h,i,j,k,l,m,n) F(a) F(b) F(c) F(d) F(e) F(f) F(g) F(h) F(i) F(j) F(k) F(l) F(m) F(n)
#define PP_FOR_EACH_15(F,a,b,c,d,e,f,g,h,i,j,k,l,m,n,o) F(a) F(b) F(c) F(d) F(e) F(f) F(g) F(h) F(i) F(j) F(k) F(l) F(m) F(n) F(o)
#define PP_FOR_EACH_16(F,a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p) F(a) F(b) F(c) F(d) F(e) F(f) F(g) F(h) F(i) F(j) F(k) F(l) F(m) F(n) F(o) F(p)

#define PP_MAP(F, ...) PP_CAT(PP_MAP_, PP_NARG(__VA_ARGS__))(F, __VA_ARGS__)
#define PP_MAP_1(F,a) F(a)
#define PP_MAP_2(F,a,b) F(a), F(b)
#define PP_MAP_3(F,a,b,c) F(a), F(b), F(c)
#define PP_MAP_4(F,a,b,c,d) F(a), F(b), F(c), F(d)
#define PP_MAP_5(F,a,b,c,d,e) F(a), F(b), F(c), F(d), F(e)
#define PP_MAP_6(F,a,b,c,d,e,f) F(a), F(b), F(c), F(d), F(e), F(f)
#define PP_MAP_7(F,a,b,c,d,e,f,g) F(a), F(b), F(c), F(d), F(e), F(f), F(g)
#define PP_MAP_8(F,a,b,c,d,e,f,g,h) F(a), F(b), F(c), F(d), F(e), F(f), F(g), F(h)
#define PP_MAP_9(F,a,b,c,d,e,f,g,h,i) F(a), F(b), F(c), F(d), F(e), F(f), F(g), F(h), F(i)
#define PP_MAP_10(F,a,b,c,d,e,f,g,h,i,j) F(a), F(b), F(c), F(d), F(e), F(f), F(g), F(h), F(i), F(j)
#define PP_MAP_11(F,a,b,c,d,e,f,g,h,i,j,k) F(a), F(b), F(c), F(d), F(e), F(f), F(g), F(h), F(i), F(j), F(k)
#define PP_MAP_12(F,a,b,c,d,e,f,g,h,i,j,k,l) F(a), F(b), F(c), F(d), F(e), F(f), F(g), F(h), F(i), F(j), F(k), F(l)
#define PP_MAP_13(F,a,b,c,d,e,f,g,h,i,j,k,l,m) F(a), F(b), F(c), F(d), F(e), F(f), F(g), F(h), F(i), F(j), F(k), F(l), F(m)
#define PP_MAP_14(F,a,b,c,d,e,f,g,h,i,j,k,l,m,n) F(a), F(b), F(c), F(d), F(e), F(f), F(g), F(h), F(i), F(j), F(k), F(l), F(m), F(n)
#define PP_MAP_15(F,a,b,c,d,e,f,g,h,i,j,k,l,m,n,o) F(a), F(b), F(c), F(d), F(e), F(f), F(g), F(h), F(i), F(j), F(k), F(l), F(m), F(n), F(o)
#define PP_MAP_16(F,a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p) F(a), F(b), F(c), F(d), F(e), F(f), F(g), F(h), F(i), F(j), F(k), F(l), F(m), F(n), F(o), F(p)

// 类元信息 + staticClass；须写在类内、先于 REFLECT_FIELDS / REFLECT_FUNCTIONS
#define REFLECT_CLASS(Type, ...)                              \
    static constexpr std::string_view reflect_name = #Type;   \
    using __reflect_self = Type;                              \
    using reflect_base = REFLECT_FIRST(__VA_ARGS__);          \
    static auto staticClass() { return core::ReflectTypeInfo<Type>{}; }

#define REFLECT_FIELD_DECL(p)  REFLECT_FIELD_DECL_I p
#define REFLECT_FIELD_DECL_I(T, n) T n;
#define REFLECT_FIELD_ENTRY(p) REFLECT_FIELD_ENTRY_I p
#define REFLECT_FIELD_ENTRY_I(T, n) core::ReflectFieldTraits{ &__reflect_self::n, #n }
#define REFLECT_FIELDS(...)                                   \
    PP_FOR_EACH(REFLECT_FIELD_DECL, __VA_ARGS__)              \
    static constexpr auto reflect_variables =                 \
        std::make_tuple(PP_MAP(REFLECT_FIELD_ENTRY, __VA_ARGS__))

#define REFLECT_FN_ENTRY(n) core::ReflectFieldTraits{ &__reflect_self::n, #n }
#define REFLECT_FUNCTIONS(...)                                \
    static constexpr auto reflect_functions =                 \
        std::make_tuple(__VA_OPT__(PP_MAP(REFLECT_FN_ENTRY, __VA_ARGS__)))

#endif // REFLECT_REFLECT_H
