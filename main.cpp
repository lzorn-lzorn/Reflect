#include <exception>
#include <iostream>
#include <type_traits>

#include "reflect.h"
#include "reflect_dynamic.h"
#include "reflect_json.h"

struct Person
{
    REFLECT_STATIC_CLASS();

    std::string name;
    int age     = 0;
    bool gender = false;

    void Introduce() const
    {
        std::cout << "Hi, I'm " << name << " and I'm " << age << " years old.\n";
    }

    int GetAge() const { return age; }

    void SetAge(int new_age) { age = new_age; }
};

BEGIN_CLASS(Person, NullBase)
    FUNCTIONS(
        FUNCTION_FIELD(&Person::Introduce),
        FUNCTION_FIELD(&Person::GetAge),
        FUNCTION_FIELD(&Person::SetAge)
    )
    VARIABLES(
        VARIABLE_FIELD(&Person::name),
        VARIABLE_FIELD(&Person::age),
        VARIABLE_FIELD(&Person::gender)
    )
END_CLASS(Person);

// 派生类：验证继承反射
struct Student : Person
{
    REFLECT_STATIC_CLASS();

    std::string school;
};

BEGIN_CLASS(Student, Person)
    FUNCTIONS()
    VARIABLES(
        VARIABLE_FIELD(&Student::school)
    )
END_CLASS(Student);

int main()
{
    // ---- 编译期校验：FunctionTraits 萃取是否正确 ----
    static_assert(std::is_same_v<Core::FunctionTraits<decltype(&Person::Introduce)>::return_type, void>);
    static_assert(Core::FunctionTraits<decltype(&Person::Introduce)>::is_const);
    static_assert(Core::FunctionTraits<decltype(&Person::Introduce)>::arity == 0);

    static_assert(!Core::FunctionTraits<decltype(&Person::SetAge)>::is_const);
    static_assert(Core::FunctionTraits<decltype(&Person::SetAge)>::arity == 1);

    static_assert(std::is_same_v<Core::FunctionTraits<decltype(&Person::GetAge)>::return_type, int>);
    static_assert(std::is_same_v<Core::FunctionTraits<decltype(&Person::GetAge)>::class_type, Person>);

    std::cout << std::boolalpha;

    // 格式无关：这里选 JSON 解析器；换 XML/YAML/INI 只需换一个 Parser 子类
    Core::JsonParser parser;

    Person alice{"Alice", 30, true};

    // 1. 类名
    std::cout << "Class name: " << Person::StaticClass().GetClassName() << "\n\n";

    // 2. 遍历成员变量
    std::cout << "Member variables:\n";
    Person::StaticClass().ForEachMembers(alice, [](const char *name, auto &value)
    {
        std::cout << "  " << name << " = " << value << "\n";
    });

    // 3. 遍历成员函数元信息
    std::cout << "\nMember functions:\n";
    Person::StaticClass().ForEachFunction([](const auto &field)
    {
        std::cout << "  " << field.name
            << " | const=" << field.IsConst()
            << " | arity=" << field.GetArity()
            << " | is_function=" << field.IsFunction() << "\n";
    });

    // 4. 反射驱动序列化（对象 -> DynamicValue -> 文本，格式由 parser 决定）
    auto dv = Core::ToDynamicObject(alice);
    std::cout << "\nSerialize (JSON):\n" << parser.Serialize(dv) << "\n";

    // 5. 继承反射：Student 应包含 Person 的字段
    Student bob{{"Bob", 18, false}, "Tsinghua"};
    std::cout << "\nStudent class: " << Student::StaticClass().GetClassName() << "\n";
    Student::StaticClass().ForEachMembers(bob, [](const char *name, auto &value)
    {
        std::cout << "  " << name << " = " << value << "\n";
    });

    // 6. 调用普通成员函数（对照）
    alice.Introduce();

    // ================= 运行时反射：字符串构造 / 字符串调用 =================
    std::cout << "\n================ Runtime reflection (string-based) ================\n";

    Core::RegisterType<Person>();
    Core::RegisterType<Student>();

    auto &reg = Core::TypeRegistry::Instance();

    // 7. 字符串 -> 默认构造对象
    auto p1                      = reg.Create("Person");
    p1.Cast<Person>().name = "Default";
    std::cout << "Create(\"Person\") -> name='" << p1.Cast<Person>().name << "'\n";

    // 8. 字符串(JSON) -> 带参构造（DynamicValue 对象按字段名赋值）
    auto p2 = reg.Create("Person", parser.Parse(R"({"name":"Carol","age":25,"gender":false})"));
    std::cout << "Create(\"Person\", {...}) -> "
        << p2.Cast<Person>().name
        << ", age=" << p2.Cast<Person>().age << "\n";

    // 9. 字符串 -> 调用无参函数（返回值 Value）
    auto age = reg.Invoke("Person", "GetAge", p2, parser.Parse("[]"));
    std::cout << "Invoke(\"GetAge\") = " << age.Cast<int>() << "\n";

    // 10. 字符串 -> 调用带参函数
    reg.Invoke("Person", "SetAge", p2, parser.Parse("[31]"));
    std::cout << "Invoke(\"SetAge\", [31]) -> age = "
        << reg.Invoke("Person", "GetAge", p2, parser.Parse("[]")).Cast<int>() << "\n";

    // 11. 字符串 -> 调用 void 函数
    std::cout << "Invoke(\"Introduce\") -> ";
    reg.Invoke("Person", "Introduce", p2, parser.Parse("[]"));

    // 12. 继承类型：构造 Student（含基类字段），并调用继承来的函数
    auto s = reg.Create("Student", parser.Parse(R"({"name":"Dave","age":20,"gender":true,"school":"PKU"})"));
    std::cout << "Student: " << s.Cast<Student>().name
        << " @ " << s.Cast<Student>().school
        << ", age=" << reg.Invoke("Student", "GetAge", s, parser.Parse("[]")).Cast<int>()
        << "\n";

    // 13. 错误处理：未知类型 / 未知函数
    try { reg.Create("NoSuchType"); }
    catch (const std::exception &e) { std::cout << "caught: " << e.what() << "\n"; }
    try { reg.Invoke("Person", "NoSuchFn", p2, parser.Parse("[]")); }
    catch (const std::exception &e) { std::cout << "caught: " << e.what() << "\n"; }

    return 0;
}
