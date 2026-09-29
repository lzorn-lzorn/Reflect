//
// Created by Lzorn on 2026/9/29.
// JSON 解析器：把 JSON 文本 <-> DynamicValue（依赖 nlohmann/json）。
//
// 这是 Parser 抽象的一个具体实现；运行时反射核心（reflect_dynamic.h）不依赖任何具体格式，
// 如需支持 XML / YAML / INI / 自定义格式，只需再实现一个 Parser 子类。
// 使用 ordered_json 以保持对象字段顺序（与 DynamicObject 的 vector<pair> 保序一致）。
//

#ifndef REFLECT_REFLECT_JSON_H
#define REFLECT_REFLECT_JSON_H
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "reflect_dynamic.h"
#include "json.hpp"

namespace Core
{

class JsonParser : public Parser
{
public:
    DynamicValue Parse(std::string_view text) const override
    {
        return FromJson(nlohmann::ordered_json::parse(text));
    }

    std::string Serialize(const DynamicValue &value) const override
    {
        return ToJson(value).dump();
    }

private:
    using Json = nlohmann::ordered_json;

    static DynamicValue FromJson(const Json &j)
    {
        if (j.is_null())            return nullptr;
        if (j.is_boolean())         return j.get<bool>();
        if (j.is_number_integer())  return j.get<std::int64_t>();
        if (j.is_number_unsigned()) return static_cast<std::int64_t>(j.get<std::uint64_t>());
        if (j.is_number_float())    return j.get<double>();
        if (j.is_string())          return j.get<std::string>();
        if (j.is_array())
        {
            DynamicArray a;
            a.reserve(j.size());
            for (const auto &e : j)
                a.push_back(FromJson(e));
            return DynamicValue{ std::move(a) };
        }
        if (j.is_object())
        {
            DynamicObject o;
            o.reserve(j.size());
            for (const auto &[k, v] : j.items())
                o.emplace_back(k, FromJson(v));
            return DynamicValue{ std::move(o) };
        }
        throw std::invalid_argument("JsonParser: unsupported json type");
    }

    static Json ToJson(const DynamicValue &v)
    {
        if (v.IsNull())   return Json(nullptr);
        if (v.IsBool())   return Json(v.As<bool>());
        if (v.IsInt())    return Json(v.As<std::int64_t>());
        if (v.IsDouble()) return Json(v.As<double>());
        if (v.IsString()) return Json(v.As<std::string>());
        if (v.IsArray())
        {
            Json a = Json::array();
            for (const auto &e : v.As<DynamicArray>())
                a.push_back(ToJson(e));
            return a;
        }
        if (v.IsObject())
        {
            Json o = Json::object();
            for (const auto &[k, e] : v.As<DynamicObject>())
                o[k] = ToJson(e);
            return o;
        }
        throw std::invalid_argument("JsonParser: unsupported dynamic value");
    }
};

} // namespace Core

#endif // REFLECT_REFLECT_JSON_H
