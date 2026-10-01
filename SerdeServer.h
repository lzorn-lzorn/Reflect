//
// SerdeServer.h — 对外提供服务的单例 + 目录(SerdeSpace)/文件(SerdeUnit)抽象
//
// 架构：外部对象 -> SerdeADT（经反射 toAdt）-> SerdeServer -> SerdeSpace(目录)
//       -> SerdeUnit(文件) -> SerdeEngine(具体后端) -> 文件系统。
// SerdeServer 维护根目录；SerdeSpace/SerdeUnit 本质是路径抽象，内部通过文件系统操作。
//

#ifndef REFLECT_SERDESERVER_H
#define REFLECT_SERDESERVER_H
#pragma once

#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "Tag.h"
#include "SerdeADT.h"
#include "SerdeEngine.h"

namespace core
{

// ========================= 序列化的目标文件（文件的抽象） =========================
class SerdeUnit
{
public:
    SerdeUnit() = default;
    SerdeUnit(std::filesystem::path path, ESerdeBackend backend)
        : Path(std::move(path)), Backend(backend) {}

    // 将 SerdeADT 经对应后端引擎写入文件
    std::expected<void, std::string> save(const SerdeADT &adt, ESerdeMode mode)
    {
        SerdeEngine *engine = engineFor(Backend);
        if (!engine) return std::unexpected("SerdeUnit::save: no engine for backend");

        std::filesystem::create_directories(Path.parent_path());
        std::ofstream ofs(Path, std::ios::out | (mode == ESerdeMode::Append ? std::ios::app : std::ios::trunc));
        if (!ofs) return std::unexpected("SerdeUnit::save: cannot open " + Path.string());

        try
        {
            ofs << engine->serialize(adt);
        }
        catch (const std::exception &e)
        {
            return std::unexpected(e.what());
        }
        return {};
    }

    // 从文件读取并经对应后端引擎解析为 SerdeADT
    std::expected<SerdeADT, std::string> load()
    {
        SerdeEngine *engine = engineFor(Backend);
        if (!engine) return std::unexpected("SerdeUnit::load: no engine for backend");

        std::ifstream ifs(Path);
        if (!ifs) return std::unexpected("SerdeUnit::load: cannot open " + Path.string());
        std::ostringstream oss;
        oss << ifs.rdbuf();

        try
        {
            return engine->parse(oss.str());
        }
        catch (const std::exception &e)
        {
            return std::unexpected(e.what());
        }
    }

    [[nodiscard]] bool isExist() const noexcept { return std::filesystem::exists(Path); }
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return Path; }
    [[nodiscard]] ESerdeBackend backend() const noexcept { return Backend; }

public:
    Tag Key; // 完整 Key, eg "Saved.Player.2026-12-8.ThisUnit"
private:
    std::filesystem::path Path;
    ESerdeBackend Backend { ESerdeBackend::None };
};

// ========================= 目录的抽象 =========================
class SerdeSpace
{
public:
    SerdeSpace() = default;
    explicit SerdeSpace(std::filesystem::path dir) : Dir(std::move(dir)) {}

    // 生成该目录下的一个 Unit 文件（dir/name.ext）
    SerdeUnit unit(std::string_view name, ESerdeBackend backend) const
    {
        std::filesystem::path p = Dir / (std::string(name) + "." + std::string(backendExtension(backend)));
        return SerdeUnit(std::move(p), backend);
    }

    [[nodiscard]] const std::filesystem::path &dir() const noexcept { return Dir; }

public:
    Tag Key; // 完整 Key, eg "Saved.Player.2026-12-8"
private:
    std::filesystem::path Dir;
};

// ========================= 对外单例 =========================
class SerdeServer
{
    constexpr static std::string_view WorkSpace = "Serde"; // 序列化根目录名

public:
    static SerdeServer &self()
    {
        static SerdeServer Instance;
        return Instance;
    }

    void setRoot(std::filesystem::path root) { Root = std::move(root); }
    [[nodiscard]] const std::filesystem::path &root() const noexcept { return Root; }

    // 根目录下的某个子目录
    SerdeSpace space(std::string_view name) const
    {
        return SerdeSpace(Root / name);
    }

    // 便捷：按 Tag（点分路径）序列化 / 反序列化
    std::expected<void, std::string>
    serialize(const SerdeADT &adt, const Tag &key, ESerdeBackend backend, ESerdeMode mode)
    {
        SerdeUnit unit(tagPath(key, backend), backend);
        return unit.save(adt, mode);
    }

    std::expected<SerdeADT, std::string>
    deserialize(const Tag &key, ESerdeBackend backend)
    {
        SerdeUnit unit(tagPath(key, backend), backend);
        return unit.load();
    }

private:
    SerdeServer() : Root(WorkSpace) {}

    // Tag -> 文件路径：Root/<各段作为目录>/<末段>.<ext>
    std::filesystem::path tagPath(const Tag &key, ESerdeBackend backend) const
    {
        std::vector<std::string> parts = split(key.get(), '.');
        std::filesystem::path p = Root;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i)
            p /= parts[i];
        p /= parts.back() + "." + std::string(backendExtension(backend));
        return p;
    }

    static std::vector<std::string> split(std::string_view s, char delim)
    {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= s.size())
        {
            auto pos = s.find(delim, start);
            parts.emplace_back(s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
            if (pos == std::string_view::npos) break;
            start = pos + 1;
        }
        return parts;
    }

private:
    std::filesystem::path Root;
};

} // namespace core

#endif // REFLECT_SERDESERVER_H
