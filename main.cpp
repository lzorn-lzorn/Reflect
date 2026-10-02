#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "SerdeADT.h"
#include "reflect.h"
#include "SerdeEngine.h"
#include "SerdeServer.h"

// ========================= 真实复杂对象（游戏存档场景） =========================
using Inventory = std::map<std::string, int>;

struct Stat
{
    REFLECT_CLASS(Stat, ReflectNullBase);
    REFLECT_FIELDS(
        (int, hp),
        (int, mp),
        (int, attack),
        (int, defense)
    );
};

struct Skill
{
    REFLECT_CLASS(Skill, ReflectNullBase);
    REFLECT_FIELDS(
        (std::string, name),
        (int,         level),
        (double,      cooldown)
    );

    void Upgrade() { ++level; }
    REFLECT_FUNCTIONS(Upgrade);
};

struct Position
{
    REFLECT_CLASS(Position, ReflectNullBase);
    REFLECT_FIELDS(
        (double, x),
        (double, y),
        (double, z)
    );
};

struct Character
{
    REFLECT_CLASS(Character, ReflectNullBase);

    REFLECT_FIELDS(
        (std::string,                name),
        (int,                        level),
        (Stat,                       baseStat),
        (std::vector<Skill>,         skills),
        (Inventory,                  inventory),
        (std::optional<std::string>, guild),
        (Position,                   position)
    );

    void LevelUp() { ++level; }
    int  GetLevel() const { return level; }
    void AddSkill(const Skill &s) { skills.push_back(s); }
    void Rename(const std::string &newName) { name = newName; }

    REFLECT_FUNCTIONS(LevelUp, GetLevel, AddSkill, Rename);
};

// ========================= 继承示例（演示基类/子类信息的记录与运行时查询） =========================
struct Entity
{
    REFLECT_CLASS(Entity, ReflectNullBase);
    REFLECT_FIELDS(
        (int, id)
    );
};

struct Player : Entity
{
    REFLECT_CLASS(Player, Entity);
    REFLECT_FIELDS(
        (std::string, nickname),
        (int,         level)
    );
};

int main()
{
    // ---- 编译期校验 ----
    static_assert(core::TypeInfo<Character>::Name == "Character");
    static_assert(std::tuple_size_v<decltype(core::TypeInfo<Character>::Variables)> == 7);
    static_assert(std::tuple_size_v<decltype(core::TypeInfo<Character>::Functions)> == 4);
    static_assert(core::ReflectFunctionTraits<decltype(&Character::GetLevel)>::IsConst);
    static_assert(core::ReflectFunctionTraits<decltype(&Character::Rename)>::Arity == 1);
    // 继承信息（编译期即可见）
    static_assert(core::TypeInfo<Player>::HasBase);
    static_assert(core::TypeInfo<Player>::BaseName == "Entity");
    static_assert(!core::TypeInfo<Character>::HasBase);
    static_assert(core::TypeInfo<Character>::BaseName.empty());

    std::cout << std::boolalpha;

    Character hero{
        "Hero", 42,
        Stat{ 1000, 500, 120, 80 },
        { Skill{ "Fireball", 3, 2.5 }, Skill{ "IceBolt", 1, 1.0 } },
        Inventory{ { "gold", 999 }, { "potion", 20 } },
        std::optional<std::string>{ "GuildOfHeroes" },
        Position{ 10.5, 20.0, -3.0 }
    };

    // ============ 一、文件测试：对象 -> SerdeADT -> 文件 -> 对象 ============
    core::SerdeADT adt = core::toAdt(hero);

    // 工作区根目录：放在当前工作目录下（而不是系统临时目录，macOS 上那是 /var/folders/… 看不见，
    // 部分环境下还无权创建），这样生成的目录与文件可以直接查看
    auto root = std::filesystem::current_path() / "SerdeDemo";
    auto &server = core::SerdeServer::self();
    server.setRoot(root);
    core::Tag key{ { "Saved", "Player", "Hero" } };

    auto rj = server.serialize(adt, key, core::ESerdeBackend::Json, core::ESerdeMode::Overwrite);
    auto rt = server.serialize(adt, key, core::ESerdeBackend::Toml, core::ESerdeMode::Overwrite);

    std::cout << "== 文件测试 ==\n"
              << "  工作区: " << std::filesystem::absolute(root).string() << "\n"
              << "  JSON 写入: " << (rj ? "ok" : rj.error()) << "  "
              << std::filesystem::absolute(root / "Saved/Player/Hero.json").string() << "\n"
              << "  TOML 写入: " << (rt ? "ok" : rt.error()) << "  "
              << std::filesystem::absolute(root / "Saved/Player/Hero.toml").string() << "\n";

    auto lj = server.deserialize(key, core::ESerdeBackend::Json);
    auto pj = lj ? core::fromAdt<Character>(*lj) : std::expected<Character, std::string>{ std::unexpect, lj.error() };
    auto lt = server.deserialize(key, core::ESerdeBackend::Toml);
    auto pt = lt ? core::fromAdt<Character>(*lt) : std::expected<Character, std::string>{ std::unexpect, lt.error() };

    std::cout << "  JSON 回环: " << (pj ? (pj->name + " lv" + std::to_string(pj->level) + " hp" + std::to_string(pj->baseStat.hp)
                                      + " gold" + std::to_string(pj->inventory["gold"]) + " guild@" + pj->guild.value_or("<none>")
                                      + " (" + std::to_string(pj->position.x) + "," + std::to_string(pj->position.y) + ")")
                                        : pj.error()) << "\n";
    std::cout << "  TOML 回环: " << (pt ? (pt->name + " lv" + std::to_string(pt->level) + " hp" + std::to_string(pt->baseStat.hp)
                                      + " skills=" + std::to_string(pt->skills.size()))
                                        : pt.error()) << "\n";

    // 落盘文本：带 \t 缩进与 \n 换行（这里直接回显文件内容）
    {
        std::ifstream Ifs(root / "Saved/Player/Hero.json");
        std::cout << "  Hero.json 内容（\\t 缩进 / \\n 换行）:\n";
        std::string Line;
        while (std::getline(Ifs, Line))
        {
            std::cout << "    " << Line << "\n";
        }
    }

    // ============ SerdeSpace / SerdeUnit ⇄ 真实目录 / 真实文件 ============
    auto space = server.space("Saved/Player");                                 // 构造即创建真实目录
    auto manual = space.createUnit("Manual", core::ESerdeBackend::Json);       // 立即落地真实文件
    std::cout << "  Space 目录: " << std::filesystem::absolute(space.dir()).string()
              << "  存在=" << space.isExist() << "\n"
              << "  Unit  文件: " << std::filesystem::absolute(manual.path()).string()
              << "  存在=" << manual.isExist() << "\n\n";

    // ============ 二、反射测试：字符串构造 / 访问成员 / 调用方法 ============
    // 不再需要手写 registerReflect<X>()：REFLECT_CLASS 展开出的 inline static 注册器
    // 已在进入 main 之前自动完成所有反射类型的注册。
    auto &reg = core::ReflectRegistry::self();

    // 1) 字符串构造对象
    auto obj = reg.create("Character", adt);
    std::cout << "== 反射测试 ==\n"
              << "  字符串构造: name=" << reg.get("Character", "name", obj).asString()
              << ", level=" << reg.get("Character", "level", obj).asInt() << "\n";

    // 2) 字符串访问成员（读）
    auto skill0 = reg.get("Character", "skills", obj);           // vector<Skill> -> ADT array
    std::cout << "  字符串访问成员: skills[0].name=" << skill0.asArray()[0].find("name")->asString()
              << ", skills 数量=" << skill0.asArray().size() << "\n";

    // 3) 字符串访问成员（写）
    reg.set("Character", "level", obj, core::SerdeADT{ std::int64_t{ 99 } });
    std::cout << "  字符串写成员: level -> " << reg.get("Character", "level", obj).asInt() << "\n";

    // 4) 字符串调用成员方法（无参 / 带参 / 返回值）
    core::SerdeADT emptyArgs{ core::SerdeArray{} };
    reg.invoke("Character", "LevelUp", obj, emptyArgs);   // 无参、void
    std::cout << "  字符串调用 LevelUp: level -> "
              << reg.invoke("Character", "GetLevel", obj, emptyArgs).asInt() << "\n";

    Skill newSkill{ "Thunder", 1, 3.0 };
    core::SerdeADT addArgs{ core::SerdeArray{ core::toAdt(newSkill) } };
    reg.invoke("Character", "AddSkill", obj, addArgs);    // 带参（Skill 对象）
    std::cout << "  字符串调用 AddSkill: skills 数量 -> "
              << reg.get("Character", "skills", obj).asArray().size() << "\n";

    reg.invoke("Character", "Rename", obj, core::SerdeADT{ core::SerdeArray{ core::SerdeADT{ "HeroRenamed" } } });
    std::cout << "  字符串调用 Rename: name -> "
              << reg.get("Character", "name", obj).asString() << "\n";

    // 5) 错误处理
    std::cout << "  错误处理:\n";
    try { (void)reg.create("NoSuchType"); }
    catch (const std::exception &e) { std::cout << "    " << e.what() << "\n"; }
    try { (void)reg.get("Character", "noSuchMember", obj); }
    catch (const std::exception &e) { std::cout << "    " << e.what() << "\n"; }

    // ============ 三、自动注册与继承信息查询 ============
    std::cout << "\n== 自动注册与继承查询 ==\n"
              << "  已自动注册类型数: " << reg.size() << "\n"
              << "  类型清单:";
    for (const auto &N : reg.typeNames()) std::cout << " " << N;
    std::cout << "\n"
              << "  直接基类: baseOf(Player) = " << reg.baseOf("Player").value_or("<none>") << "\n"
              << "  基类链: baseChainOf(Player) = [";
    for (const auto &B : reg.baseChainOf("Player")) std::cout << " " << B;
    std::cout << " ]\n"
              << "  直接子类: derivedOf(Entity) = [";
    for (const auto &D : reg.derivedOf("Entity")) std::cout << " " << D;
    std::cout << " ]\n"
              << "  isDerivedFrom(Player, Entity)   = " << std::boolalpha << reg.isDerivedFrom("Player", "Entity") << "\n"
              << "  isDerivedFrom(Character, Entity) = " << reg.isDerivedFrom("Character", "Entity") << "\n";

    // 继承字段参与反射遍历：Player 的成员包含基类 Entity 的 id
    const core::SerdeADT playerAdt = core::toAdt(Player{ { 7 }, "Bilbo", 3 });
    std::cout << "  继承字段遍历: id=" << playerAdt.find("id")->asInt()
              << ", nickname=" << playerAdt.find("nickname")->asString()
              << ", level=" << playerAdt.find("level")->asInt() << "\n";

    // ============ 四、工作区落盘结果（磁盘上真实存在的目录与文件） ============
    std::cout << "\n== 工作区实际内容 ==\n  " << std::filesystem::absolute(root).string() << "\n";
    if (std::filesystem::exists(root))
    {
        for (const auto &Entry : std::filesystem::recursive_directory_iterator(root))
        {
            const auto Rel = std::filesystem::relative(Entry.path(), root).generic_string();
            if (Entry.is_directory()) std::cout << "  [dir ] " << Rel << "\n";
            else                      std::cout << "  [file] " << Rel << "  (" << Entry.file_size() << " B)\n";
        }
    }
    else
    {
        std::cout << "  (未生成)\n";
    }

    return 0;
}
