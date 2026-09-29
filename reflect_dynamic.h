//
// Created by Lzorn on 2026/9/29.
// 格式无关的“运行时反射”核心：
//   - DynamicValue：std::variant 表示的动态值树（与具体序列化格式解耦）
//   - Parser：解析器抽象，负责「文本 <-> DynamicValue」双向转换
//   - TypeRegistry：全局注册表，字符串 -> 构造对象 / 调用成员函数
//
// 本头不依赖任何具体格式（无 nlohmann/json）；具体格式由 Parser 子类提供（见 reflect_json.h）。
//

#ifndef REFLECT_REFLECT_DYNAMIC_H
#define REFLECT_REFLECT_DYNAMIC_H
#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo> // std::bad_cast
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "reflect.h"
#include "reflect_value.h"

namespace Core
{

// ========================= 动态值（std::variant，递归） =========================
struct DynamicValue;

using DynamicArray  = std::vector<DynamicValue>;
using DynamicObject = std::vector<std::pair<std::string, DynamicValue>>; // 保序

struct DynamicValue : std::variant<
                          std::nullptr_t,
                          bool,
                          std::int64_t,
                          double,
                          std::string,
                          DynamicArray,
                          DynamicObject>
{
    using Base = std::variant<
        std::nullptr_t, bool, std::int64_t, double, std::string, DynamicArray, DynamicObject>;
    using Base::Base;

    [[nodiscard]] bool IsNull()   const { return std::holds_alternative<std::nullptr_t>(*this); }
    [[nodiscard]] bool IsBool()   const { return std::holds_alternative<bool>(*this); }
    [[nodiscard]] bool IsInt()    const { return std::holds_alternative<std::int64_t>(*this); }
    [[nodiscard]] bool IsDouble() const { return std::holds_alternative<double>(*this); }
    [[nodiscard]] bool IsString() const { return std::holds_alternative<std::string>(*this); }
    [[nodiscard]] bool IsArray()  const { return std::holds_alternative<DynamicArray>(*this); }
    [[nodiscard]] bool IsObject() const { return std::holds_alternative<DynamicObject>(*this); }

    template<typename T> [[nodiscard]] const T &As() const { return std::get<T>(*this); }
    template<typename T> T &As() { return std::get<T>(*this); }
};

// ========================= DynamicValue <-> 标量类型 =========================
// 用于把动态值转成字段类型 / 函数实参类型（类型不符抛 std::bad_cast）
template<typename T>
T ValueCast(const DynamicValue &v)
{
    if constexpr (std::is_same_v<T, std::string>)
    {
        if (!v.IsString()) throw std::bad_cast();
        return v.As<std::string>();
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        if (!v.IsBool()) throw std::bad_cast();
        return v.As<bool>();
    }
    else if constexpr (std::is_integral_v<T>)
    {
        if (v.IsInt())    return static_cast<T>(v.As<std::int64_t>());
        if (v.IsDouble()) return static_cast<T>(v.As<double>());
        throw std::bad_cast();
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        if (v.IsDouble()) return static_cast<T>(v.As<double>());
        if (v.IsInt())    return static_cast<T>(v.As<std::int64_t>());
        throw std::bad_cast();
    }
    else
    {
        static_assert(std::is_same_v<T, void>, "ValueCast: unsupported scalar type");
        throw std::bad_cast();
    }
}

// 标量 -> DynamicValue（序列化方向）
template<typename T>
DynamicValue ToDynamicScalar(const T &v)
{
    if constexpr (std::is_same_v<T, bool>)
        return DynamicValue{ v };
    else if constexpr (std::is_same_v<T, std::string>)
        return DynamicValue{ v };
    else if constexpr (std::is_integral_v<T>)
        return DynamicValue{ static_cast<std::int64_t>(v) };
    else if constexpr (std::is_floating_point_v<T>)
        return DynamicValue{ static_cast<double>(v) };
    else
        return DynamicValue{ nullptr };
}

// 对象 -> DynamicValue(object)：遍历字段（含继承）
template<typename T>
DynamicValue ToDynamicObject(const T &obj)
{
    DynamicObject members;
    TypeInfo<T>::ForEachMembers(obj, [&members](const char *name, const auto &value) {
        members.emplace_back(name, ToDynamicScalar(value));
    });
    return DynamicValue{ std::move(members) };
}

// ========================= 解析器抽象 =========================
class Parser
{
public:
    virtual ~Parser() = default;
    // 任意格式文本 -> 动态值
    [[nodiscard]] virtual DynamicValue Parse(std::string_view text) const = 0;
    // 动态值 -> 该格式文本
    [[nodiscard]] virtual std::string Serialize(const DynamicValue &value) const = 0;
};

// ========================= 参数元组：去掉成员函数首元素（隐式 this） =========================
template<typename Tuple>
struct ArgsTuple;

template<typename First, typename... Rest>
struct ArgsTuple<std::tuple<First, Rest...>>
{
    using type = std::tuple<Rest...>;
};

// ========================= 类型擦除调用：DynamicValue(数组) -> 成员函数 =========================
template<typename Class, typename MemFn, typename ReturnType, typename... Args>
Value InvokeMemberImpl(Class &self, MemFn fn, const DynamicValue &args, std::tuple<Args...>)
{
    if (!args.IsArray())
        throw std::invalid_argument("InvokeMember: arguments must be an array");
    const auto &arr = args.As<DynamicArray>();
    if (arr.size() != sizeof...(Args))
        throw std::invalid_argument("InvokeMember: argument count mismatch");

    return [&]<std::size_t... I>(std::index_sequence<I...>) -> Value {
        if constexpr (std::is_void_v<ReturnType>)
        {
            (self.*fn)(ValueCast<std::remove_cvref_t<Args>>(arr[I])...);
            return Value{};
        }
        else
        {
            return Value{ (self.*fn)(ValueCast<std::remove_cvref_t<Args>>(arr[I])...) };
        }
    }(std::index_sequence_for<Args...>{});
}

template<typename Class, typename MemFn>
Value InvokeMember(Class &self, MemFn fn, const DynamicValue &args)
{
    using traits        = FunctionTraits<MemFn>;
    using explicit_args = typename ArgsTuple<typename traits::argument_types>::type;
    return InvokeMemberImpl<Class, MemFn, typename traits::return_type>(self, fn, args, explicit_args{});
}

// ========================= 全局类型注册表 =========================
class TypeRegistry
{
public:
    // 工厂：DynamicValue -> 对象（空值即默认构造）
    using Factory = std::function<Value(const DynamicValue &)>;
    // 调用器：对象 + DynamicValue 参数 -> 返回值（void 返回空 Value）
    using Invoker = std::function<Value(Value &, const DynamicValue &)>;

    static TypeRegistry &Instance()
    {
        static TypeRegistry instance;
        return instance;
    }

    void Register(std::string_view name, Factory factory,
                  std::unordered_map<std::string, Invoker> functions)
    {
        types_.emplace(std::string(name), Entry{ std::move(factory), std::move(functions) });
    }

    bool Has(std::string_view name) const
    {
        return types_.find(std::string(name)) != types_.end();
    }

    // 字符串构造对象（默认构造）
    Value Create(std::string_view name) const
    {
        return Create(name, DynamicValue{ nullptr });
    }

    // 字符串构造对象（带参：DynamicValue 对象按字段名赋值）
    Value Create(std::string_view name, const DynamicValue &args) const
    {
        auto it = types_.find(std::string(name));
        if (it == types_.end())
            throw std::invalid_argument("TypeRegistry: unknown type '" + std::string(name) + "'");
        return it->second.factory(args);
    }

    // 字符串调用成员函数：obj 由 Create 得到；args 为 DynamicValue 数组（按位置传参）
    Value Invoke(std::string_view type, std::string_view fn,
                 Value &obj, const DynamicValue &args) const
    {
        auto it = types_.find(std::string(type));
        if (it == types_.end())
            throw std::invalid_argument("TypeRegistry: unknown type '" + std::string(type) + "'");

        auto fit = it->second.functions.find(std::string(fn));
        if (fit == it->second.functions.end())
            throw std::invalid_argument("TypeRegistry: unknown function '" + std::string(fn) + "'");

        return fit->second(obj, args);
    }

private:
    struct Entry
    {
        Factory factory;
        std::unordered_map<std::string, Invoker> functions;
    };

    std::unordered_map<std::string, Entry> types_;
};

// ========================= 由 TypeInfo<T> 生成工厂 / 调用表 =========================
template<typename T>
Value CreateFromValue(const DynamicValue &args)
{
    T obj{};
    if (args.IsObject())
    {
        const auto &members = args.As<DynamicObject>();
        TypeInfo<T>::ForEachMembers(obj, [&members](const char *name, auto &value) {
            for (const auto &[k, v] : members)
            {
                if (k == name)
                {
                    value = ValueCast<std::decay_t<decltype(value)>>(v);
                    break;
                }
            }
        });
    }
    return Value{ std::move(obj) };
}

template<typename T>
std::unordered_map<std::string, TypeRegistry::Invoker> BuildInvokers()
{
    std::unordered_map<std::string, TypeRegistry::Invoker> table;
    TypeInfo<T>::ForEachFunction([&table](const auto &field) {
        table[std::string(field.name)] =
            [ptr = field.pointer](Value &obj, const DynamicValue &args) -> Value {
                return InvokeMember(obj.Cast<T>(), ptr, args);
            };
    });
    return table;
}

// 便捷入口：把类型注册进全局表
template<typename T>
void RegisterType()
{
    TypeRegistry::Instance().Register(TypeInfo<T>::name, &CreateFromValue<T>, BuildInvokers<T>());
}

} // namespace Core

#endif // REFLECT_REFLECT_DYNAMIC_H
