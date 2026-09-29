//
// Created by Lzorn on 2026/9/29.
// 一个极简的 C++20 静态反射辅助库：
//   1. 注册类的成员变量 / 成员函数（含名字）
//   2. ForEachMembers 遍历成员变量（支持继承）
//   3. ForEachFunction 遍历成员函数元信息
//   4. FunctionTraits / VariableTraits 萃取类型信息
//

#ifndef REFLECT_REFLECT_H
#define REFLECT_REFLECT_H
#pragma once

#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Core
{

// ========================= 前置声明 / 类型标识 =========================
// TypeInfo<T> 同时充当“每类型唯一标识”的载体：取其静态成员 type_id 的地址即可
// 作为运行时类型 ID（不依赖 RTTI）。
//   - 反射类型：在 BEGIN_CLASS 特化里定义自己的 type_id；
//   - 非反射类型（如 int、std::string 等返回值）：走主模板拿到默认 type_id。
template<typename Ty>
struct TypeInfo
{
    static constexpr char type_id{};
};

// 继承链的终止哨兵
struct NullBase
{
};

template<>
struct TypeInfo<NullBase>
{
    static constexpr char type_id{};

    using self_type                 = NullBase;
    using base_type                 = NullBase;
    static constexpr bool has_base  = false;
    static constexpr std::string_view name = "NullBase";

    static constexpr auto functions = std::tuple{};
    static constexpr auto variables = std::tuple{};

    static constexpr const char *GetClassName() { return "NullBase"; }

    template<typename Fn>
    static void ForEachMembers(const NullBase &, Fn &&) {}

    template<typename Fn>
    static void ForEachFunction(Fn &&) {}
};

// ========================= 成员指针的类类型萃取 =========================
template<typename Ty>
struct MemberPointerClass;

template<typename Class, typename Ty>
struct MemberPointerClass<Ty Class::*>
{
    using type = Class;
};

template<typename Ty>
using MemberPointerClass_t = typename MemberPointerClass<Ty>::type;

// ========================= 变量类型萃取 =========================
template<typename Ty>
struct VariableType
{
    using original_type = Ty;
    using pure_type     = std::remove_cvref_t<Ty>;
};

template<typename Class, typename Ty>
struct VariableType<Ty Class::*>
{
    using original_type = Ty;
    using pure_type     = std::remove_cvref_t<Ty>;
};

// ========================= FunctionTraits =========================
// 主模板不定义，仅对函数类型特化；非函数类型不应直接实例化
template<typename Ty>
struct FunctionTraits;

// 自由函数
template<typename ReturnType, typename... Args>
struct FunctionTraits<ReturnType(Args...)>
{
    using return_type    = ReturnType;
    using argument_types = std::tuple<Args...>;
    using class_type     = void;

    static constexpr std::size_t arity        = sizeof...(Args);
    static constexpr bool is_member_function  = false;
    static constexpr bool is_const            = false;
    static constexpr bool is_function         = true;
    static constexpr bool is_variable         = false;
};

// 非 const 成员函数
template<typename ReturnType, typename Class, typename... Args>
struct FunctionTraits<ReturnType(Class::*)(Args...)>
{
    using return_type    = ReturnType;
    using argument_types = std::tuple<Class *, Args...>;
    using class_type     = Class;

    static constexpr std::size_t arity              = sizeof...(Args);
    static constexpr bool is_member_function = true;
    static constexpr bool is_const            = false;
    static constexpr bool is_function         = true;
    static constexpr bool is_variable         = false;
};

// const 成员函数
template<typename ReturnType, typename Class, typename... Args>
struct FunctionTraits<ReturnType(Class::*)(Args...) const>
{
    using return_type    = ReturnType;
    using argument_types = std::tuple<const Class *, Args...>;
    using class_type     = Class;

    static constexpr std::size_t arity              = sizeof...(Args);
    static constexpr bool is_member_function = true;
    static constexpr bool is_const            = true;
    static constexpr bool is_function         = true;
    static constexpr bool is_variable         = false;
};

// 注意：如需支持 noexcept / 引用限定（&、&&），按同样方式追加特化即可。

// ========================= VariableTraits =========================
template<typename Ty>
struct VariableTraits
{
    using original_type = typename VariableType<Ty>::original_type;
    using pure_type     = typename VariableType<Ty>::pure_type;
    using class_type    = void;
    using return_type   = void;
    using argument_types = std::tuple<>;

    static constexpr std::size_t arity = 0;
    static constexpr bool is_member_function = false;
    static constexpr bool is_const            = false;
    static constexpr bool is_function         = false;
    static constexpr bool is_variable         = true;

    static constexpr bool is_member_pointer = false;
    static constexpr bool is_pointer        = std::is_pointer_v<Ty>;
    static constexpr bool is_reference      = std::is_reference_v<Ty>;
};

// 数据成员指针
template<typename Ty>
    requires std::is_member_object_pointer_v<Ty>
struct VariableTraits<Ty>
{
    using original_type = typename VariableType<Ty>::original_type;
    using pure_type     = typename VariableType<Ty>::pure_type;
    using class_type    = MemberPointerClass_t<Ty>;
    using return_type   = void;
    using argument_types = std::tuple<>;

    static constexpr std::size_t arity = 0;
    static constexpr bool is_member_function = false;
    static constexpr bool is_const            = std::is_const_v<std::remove_reference_t<original_type>>;
    static constexpr bool is_function         = false;
    static constexpr bool is_variable         = true;

    static constexpr bool is_member_pointer = true;
    static constexpr bool is_pointer        = false;
    static constexpr bool is_reference      = false;
};

// ========================= BasicFieldTraits =========================
// 统一字段描述的基础属性；通过 std::conditional_t 惰性选择函数/变量萃取，避免
// 实例化错误分支（原实现直接在条件表达式中访问 FunctionTraits<Ty>::arity 会
// 导致对非函数类型实例化不完整模板的编译错误）。
template<typename Ty>
struct BasicFieldTraits
{
private:
    static constexpr bool is_mem_fn = std::is_member_function_pointer_v<Ty>;
    using traits = std::conditional_t<is_mem_fn, FunctionTraits<Ty>, VariableTraits<Ty>>;

public:
    using return_type    = typename traits::return_type;
    using argument_types = typename traits::argument_types;
    using class_type     = typename traits::class_type;

    static constexpr std::size_t arity = traits::arity;
    static constexpr bool is_member_function = traits::is_member_function;
    static constexpr bool is_const            = traits::is_const;
    static constexpr bool is_function         = traits::is_function;
    static constexpr bool is_variable         = traits::is_variable;

    constexpr bool IsMemberFunction() const noexcept { return is_member_function; }
    constexpr bool IsConst() const noexcept { return is_const; }
    constexpr std::size_t GetArity() const noexcept { return arity; }
    constexpr bool IsFunction() const noexcept { return is_function; }
    constexpr bool IsVariable() const noexcept { return is_variable; }
};

// ========================= FieldTraits =========================
template<typename Ty>
struct FieldTraits : BasicFieldTraits<Ty>
{
    Ty pointer;
    std::string_view name;

    // "&Person::age" -> "age"
    static constexpr std::string_view RealName(std::string_view raw) noexcept
    {
        auto pos = raw.find_last_of(':');
        return (pos == std::string_view::npos) ? raw : raw.substr(pos + 1);
    }

    constexpr FieldTraits(Ty ptr, std::string_view raw_name) noexcept
        : pointer(ptr),
          name(RealName(raw_name))
    {
    }
};

template<typename Ty>
FieldTraits(Ty, std::string_view) -> FieldTraits<Ty>;

} // namespace Core

// ========================= 注册宏 =========================
// 取第一个实参；若为空则回退到 NullBase
#define CORE_FIRST_SELECT(first, ...) first
#define CORE_FIRST(...) CORE_FIRST_SELECT(__VA_ARGS__ __VA_OPT__(,) Core::NullBase)

// 在类内声明 StaticClass，END_CLASS 负责定义
#define REFLECT_STATIC_CLASS() static auto StaticClass()

// 展开为 TypeInfo<Ty> 的特化体；成员在 FUNCTIONS / VARIABLES / END_CLASS 中补齐
#define BEGIN_CLASS(Type, ...)                                                            \
    template <>                                                                           \
    struct Core::TypeInfo<Type>                                                           \
    {                                                                                     \
        static const inline char type_id{};                                               \
        using self_type                 = Type;                                           \
        using base_type                 = CORE_FIRST(__VA_ARGS__);                        \
        static constexpr bool has_base  = !std::is_same_v<base_type, Core::NullBase>;     \
        static_assert(std::is_same_v<base_type, Core::NullBase> || std::is_class_v<base_type>, \
                      "Base must be a class type or NullBase");                           \
        static constexpr std::string_view name = #Type;

#define FUNCTION_FIELD(Fn)  Core::FieldTraits{ Fn, #Fn }
#define VARIABLE_FIELD(Var) Core::FieldTraits{ Var, #Var }

// 注意：这里只登记“本类自身”的成员，基类成员由 ForEachMembers / ForEachFunction
// 沿继承链递归收集，避免基类成员被重复遍历。
#define FUNCTIONS(...)                                                                    \
        static constexpr auto functions = std::make_tuple(__VA_ARGS__);

#define VARIABLES(...)                                                                    \
        static constexpr auto variables = std::make_tuple(__VA_ARGS__);

#define END_CLASS(SelfType)                                                               \
        static constexpr const char *GetClassName() { return name.data(); }               \
        template<typename Fn>                                                             \
        static void ForEachMembers(SelfType &obj, Fn &&fn)                                \
        {                                                                                 \
            if constexpr (has_base)                                                       \
                Core::TypeInfo<base_type>::ForEachMembers(obj, std::forward<Fn>(fn));     \
            std::apply([&](auto &...field) {                                              \
                (fn(field.name.data(), obj.*(field.pointer)), ...);                       \
            }, variables);                                                                \
        }                                                                                 \
        template<typename Fn>                                                             \
        static void ForEachMembers(const SelfType &obj, Fn &&fn)                          \
        {                                                                                 \
            if constexpr (has_base)                                                       \
                Core::TypeInfo<base_type>::ForEachMembers(obj, std::forward<Fn>(fn));     \
            std::apply([&](auto &...field) {                                              \
                (fn(field.name.data(), obj.*(field.pointer)), ...);                       \
            }, variables);                                                                \
        }                                                                                 \
        template<typename Fn>                                                             \
        static void ForEachFunction(Fn &&fn)                                              \
        {                                                                                 \
            if constexpr (has_base)                                                       \
                Core::TypeInfo<base_type>::ForEachFunction(std::forward<Fn>(fn));         \
            std::apply([&](auto &...field) { (fn(field), ...); }, functions);             \
        }                                                                                 \
    };                                                                                    \
    inline auto SelfType::StaticClass() { return Core::TypeInfo<SelfType>{}; }

#endif // REFLECT_REFLECT_H
