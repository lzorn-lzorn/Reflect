//
// SerdeEngine.h — 序列化后端引擎
//
// SerdeEngine 是“SerdeADT <-> 文本”的抽象后端，与反射、业务类型完全解耦。
// 具体格式（Json / Toml）各自实现一个引擎；SerdeServer 通过 engineFor() 按后端类型分派。
//

#ifndef REFLECT_SERDEENGINE_H
#define REFLECT_SERDEENGINE_H
#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "SerdeADT.h"
#include "json.hpp"
#include "toml.hpp"

namespace core
{

// ========================= 模式 / 后端类型 =========================
enum class ESerdeMode
{
    Append,
    Overwrite
};

enum class ESerdeBackend
{
    None,   // 初始化空状态
    Json,   // 将 SerdeADT 转化为 Json 文件
    Toml,   // 将 SerdeADT 转化为 Toml 文件
    Binary, // 直接将 SerdeADT 转化为二进制文件
};

inline std::string_view backendExtension(ESerdeBackend backend)
{
    switch (backend)
    {
        case ESerdeBackend::Json:   return "json";
        case ESerdeBackend::Toml:   return "toml";
        case ESerdeBackend::Binary: return "bin";
        default:                    return "";
    }
}

// ========================= base64 / ISO8601 工具 =========================
inline constexpr std::string_view kBase64Chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64Encode(std::string_view bytes)
{
    const auto *p = reinterpret_cast<const std::uint8_t *>(bytes.data());
    const std::size_t n = bytes.size();
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3)
    {
        std::uint32_t v = (static_cast<std::uint32_t>(p[i]) << 16) |
                          (static_cast<std::uint32_t>(p[i + 1]) << 8) |
                          static_cast<std::uint32_t>(p[i + 2]);
        out.push_back(kBase64Chars[(v >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(v >> 12) & 0x3F]);
        out.push_back(kBase64Chars[(v >> 6) & 0x3F]);
        out.push_back(kBase64Chars[v & 0x3F]);
    }
    const std::size_t rem = n - i;
    if (rem == 1)
    {
        std::uint32_t v = static_cast<std::uint32_t>(p[i]) << 16;
        out.push_back(kBase64Chars[(v >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    }
    else if (rem == 2)
    {
        std::uint32_t v = (static_cast<std::uint32_t>(p[i]) << 16) |
                          (static_cast<std::uint32_t>(p[i + 1]) << 8);
        out.push_back(kBase64Chars[(v >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(v >> 12) & 0x3F]);
        out.push_back(kBase64Chars[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

inline std::optional<SerdeBinary> base64Decode(std::string_view in)
{
    static std::int8_t table[256] = {};
    static bool init = [] {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(kBase64Chars[i])] = static_cast<std::int8_t>(i);
        return true;
    }();
    (void)init;

    SerdeBinary out;
    out.reserve(in.size() / 4 * 3 + 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : in)
    {
        if (c == '=' || c == '\n' || c == '\r') continue;
        auto v = table[static_cast<unsigned char>(c)];
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<std::byte>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

inline std::string toIso8601(std::chrono::system_clock::time_point tp)
{
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

inline std::optional<std::chrono::system_clock::time_point> fromIso8601(std::string_view s)
{
    std::tm tm{};
    std::istringstream iss{ std::string(s) };
    iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (iss.fail()) return std::nullopt;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

// ========================= 抽象后端 =========================
class SerdeEngine
{
public:
    virtual ~SerdeEngine() = default;
    virtual SerdeADT parse(std::string_view text) const = 0;
    virtual std::string serialize(const SerdeADT &adt) const = 0;
};

// ========================= JSON 后端 =========================
class SerdeJsonEngine : public SerdeEngine
{
    using Json = nlohmann::ordered_json;

public:
    SerdeADT parse(std::string_view text) const override
    {
        try
        {
            return fromJson(Json::parse(text));
        }
        catch (const std::exception &e)
        {
            throw std::runtime_error(std::string("JSON parse error: ") + e.what());
        }
    }

    std::string serialize(const SerdeADT &adt) const override
    {
        return toJson(adt).dump();
    }

private:
    static Json toJson(const SerdeADT &v)
    {
        if (v.isNull())   return Json(nullptr);
        if (v.isBool())   return Json(v.asBool());
        if (v.isInt())    return Json(v.asInt());
        if (v.isUint())   return Json(v.asUint());
        if (v.isDouble()) return Json(v.asDouble());
        if (v.isString()) return Json(v.asString());
        if (v.isBinary())
        {
            Json o = Json::object();
            o["$binary"] = base64Encode({ reinterpret_cast<const char *>(v.asBinary().data()),
                                          v.asBinary().size() });
            return o;
        }
        if (v.isDatetime())
        {
            Json o = Json::object();
            o["$datetime"] = toIso8601(v.asDatetime());
            return o;
        }
        if (v.isArray())
        {
            Json a = Json::array();
            for (const auto &e : v.asArray()) a.push_back(toJson(e));
            return a;
        }
        if (v.isObject())
        {
            Json o = Json::object();
            for (const auto &[k, e] : v.asObject()) o[k] = toJson(e);
            return o;
        }
        throw std::runtime_error("toJson: unsupported value");
    }

    static SerdeADT fromJson(const Json &j)
    {
        if (j.is_null())              return SerdeADT{ nullptr };
        if (j.is_boolean())           return SerdeADT{ j.get<bool>() };
        if (j.is_number_integer())    return SerdeADT{ j.get<std::int64_t>() };
        if (j.is_number_unsigned())   return SerdeADT{ j.get<std::uint64_t>() };
        if (j.is_number_float())      return SerdeADT{ j.get<double>() };
        if (j.is_string())            return SerdeADT{ j.get<std::string>() };
        if (j.is_array())
        {
            SerdeArray a;
            for (const auto &e : j) a.push_back(fromJson(e));
            return SerdeADT{ std::move(a) };
        }
        if (j.is_object())
        {
            if (j.size() == 1)
            {
                if (auto it = j.find("$binary"); it != j.end() && it->is_string())
                {
                    if (auto dec = base64Decode(it->get<std::string>()))
                        return SerdeADT{ std::move(*dec) };
                }
                if (auto it = j.find("$datetime"); it != j.end() && it->is_string())
                {
                    if (auto tp = fromIso8601(it->get<std::string>()))
                        return SerdeADT{ *tp };
                }
            }
            SerdeObject o;
            for (const auto &[k, e] : j.items()) o.emplace_back(k, fromJson(e));
            return SerdeADT{ std::move(o) };
        }
        throw std::runtime_error("fromJson: unsupported json value");
    }
};

// ========================= TOML 后端 =========================
class SerdeTomlEngine : public SerdeEngine
{
public:
    SerdeADT parse(std::string_view text) const override
    {
        try
        {
            toml::table tbl = toml::parse(text); // TOML_EXCEPTIONS=1 时 parse 直接返回 table，失败抛异常
            return fromTomlTable(tbl);
        }
        catch (const std::exception &e)
        {
            throw std::runtime_error(std::string("TOML parse error: ") + e.what());
        }
    }

    std::string serialize(const SerdeADT &adt) const override
    {
        if (!adt.isObject()) throw std::runtime_error("TOML root must be a table");
        toml::table tbl = toTomlTable(adt.asObject());
        std::ostringstream oss;
        oss << tbl;
        return oss.str();
    }

private:
    static void insertToml(toml::table &tbl, const std::string &key, const SerdeADT &v)
    {
        if (v.isString())      { tbl.insert_or_assign(key, v.asString()); }
        else if (v.isInt())    { tbl.insert_or_assign(key, v.asInt()); }
        else if (v.isUint())   { tbl.insert_or_assign(key, static_cast<std::int64_t>(v.asUint())); }
        else if (v.isDouble()) { tbl.insert_or_assign(key, v.asDouble()); }
        else if (v.isBool())   { tbl.insert_or_assign(key, v.asBool()); }
        else if (v.isArray())
        {
            toml::array arr;
            for (const auto &e : v.asArray())
            {
                if (e.isObject())
                {
                    toml::table sub;
                    for (const auto &[k2, e2] : e.asObject()) insertToml(sub, k2, e2);
                    arr.push_back(std::move(sub));
                }
                else if (e.isString())      arr.push_back(e.asString());
                else if (e.isInt())         arr.push_back(e.asInt());
                else if (e.isUint())        arr.push_back(static_cast<std::int64_t>(e.asUint()));
                else if (e.isDouble())      arr.push_back(e.asDouble());
                else if (e.isBool())        arr.push_back(e.asBool());
                // null / binary / datetime：TOML 无原生支持，跳过
            }
            tbl.insert_or_assign(key, std::move(arr));
        }
        else if (v.isObject())
        {
            toml::table sub;
            for (const auto &[k2, e2] : v.asObject()) insertToml(sub, k2, e2);
            tbl.insert_or_assign(key, std::move(sub));
        }
        // null / binary / datetime：跳过
    }

    static toml::table toTomlTable(const SerdeObject &obj)
    {
        toml::table tbl;
        for (const auto &[k, v] : obj) insertToml(tbl, k, v);
        return tbl;
    }

    static SerdeADT fromTomlNode(const toml::node &node)
    {
        if (node.is_string())         return SerdeADT{ node.value<std::string>().value_or("") };
        if (node.is_integer())        return SerdeADT{ node.value<std::int64_t>().value_or(0) };
        if (node.is_floating_point()) return SerdeADT{ node.value<double>().value_or(0.0) };
        if (node.is_boolean())        return SerdeADT{ node.value<bool>().value_or(false) };
        if (node.is_array())
        {
            SerdeArray arr;
            for (const auto &e : *node.as_array()) arr.push_back(fromTomlNode(e));
            return SerdeADT{ std::move(arr) };
        }
        if (node.is_table())
        {
            SerdeObject obj;
            for (const auto &[k, v] : *node.as_table()) obj.emplace_back(k, fromTomlNode(v));
            return SerdeADT{ std::move(obj) };
        }
        if (node.is_date_time())
        {
            std::ostringstream oss;
            oss << *node.as_date_time();
            return SerdeADT{ oss.str() };
        }
        return SerdeADT{ nullptr };
    }

    static SerdeADT fromTomlTable(const toml::table &tbl)
    {
        SerdeObject obj;
        for (const auto &[k, v] : tbl) obj.emplace_back(k, fromTomlNode(v));
        return SerdeADT{ std::move(obj) };
    }
};

// ========================= 后端工厂 =========================
inline SerdeEngine *engineFor(ESerdeBackend backend)
{
    static SerdeJsonEngine jsonEngine;
    static SerdeTomlEngine tomlEngine;
    switch (backend)
    {
        case ESerdeBackend::Json: return &jsonEngine;
        case ESerdeBackend::Toml: return &tomlEngine;
        default:                  return nullptr;
    }
}

} // namespace core

#endif // REFLECT_SERDEENGINE_H
