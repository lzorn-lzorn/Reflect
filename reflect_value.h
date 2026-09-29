//
// Created by Lzorn on 2026/9/29.
// 反射运行时内部使用的类型擦除值容器（Value），零 RTTI 依赖。
//
// 类型识别复用 TypeInfo<T>::type_id 的地址作为运行时类型 ID（不依赖 typeid/dynamic_cast），
// 因此可以在 -fno-rtti 下正常工作。仅用于反射运行时装箱对象与函数返回值。
//

#ifndef REFLECT_REFLECT_VALUE_H
#define REFLECT_REFLECT_VALUE_H
#pragma once

#include <cstddef>
#include <type_traits>
#include <typeinfo> // std::bad_cast（仅异常类型，与 RTTI 无关）
#include <utility>  // std::forward / std::move

#include "reflect.h"

namespace Core
{

class Value
{
public:
    Value() noexcept = default;

    // 从任意值构造（拷贝或移动进堆），排除 Value 自身以避免与拷贝/移动构造冲突
    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, Value>)
    Value(T &&value)
    {
        using U = std::decay_t<T>;
        ptr_    = new U(std::forward<T>(value));
        id_     = &TypeInfo<U>::type_id;
        vtable_ = &VTable<U>::get();
    }

    Value(const Value &other)
    {
        if (other.ptr_)
        {
            ptr_    = other.vtable_->clone(other.ptr_);
            id_     = other.id_;
            vtable_ = other.vtable_;
        }
    }

    Value(Value &&other) noexcept
    {
        ptr_         = other.ptr_;
        id_          = other.id_;
        vtable_      = other.vtable_;
        other.ptr_    = nullptr;
        other.id_     = nullptr;
        other.vtable_ = nullptr;
    }

    Value &operator=(const Value &other)
    {
        if (this != &other)
        {
            reset();
            if (other.ptr_)
            {
                ptr_    = other.vtable_->clone(other.ptr_);
                id_     = other.id_;
                vtable_ = other.vtable_;
            }
        }
        return *this;
    }

    Value &operator=(Value &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            ptr_         = other.ptr_;
            id_          = other.id_;
            vtable_      = other.vtable_;
            other.ptr_    = nullptr;
            other.id_     = nullptr;
            other.vtable_ = nullptr;
        }
        return *this;
    }

    ~Value() { reset(); }

    bool HasValue() const noexcept { return ptr_ != nullptr; }
    void Reset() noexcept { reset(); }

    template<typename T>
    bool Is() const noexcept
    {
        return ptr_ != nullptr && id_ == &TypeInfo<std::decay_t<T>>::type_id;
    }

    // 就地构造并返回引用
    template<typename T, typename... Args>
    T &Emplace(Args &&...args)
    {
        reset();
        using U = std::decay_t<T>;
        ptr_    = new U(std::forward<Args>(args)...);
        id_     = &TypeInfo<U>::type_id;
        vtable_ = &VTable<U>::get();
        return *static_cast<U *>(ptr_);
    }

    // 类型安全的取出（类型不匹配抛 std::bad_cast）
    template<typename T>
    T &Cast()
    {
        using U = std::decay_t<T>;
        if (!Is<U>())
            throw std::bad_cast();
        return *static_cast<U *>(ptr_);
    }

    template<typename T>
    const T &Cast() const
    {
        using U = std::decay_t<T>;
        if (!Is<U>())
            throw std::bad_cast();
        return *static_cast<const U *>(ptr_);
    }

private:
    struct VTableBase
    {
        void *(*clone)(const void *);
        void (*destroy)(void *);
    };

    template<typename T>
    struct VTable
    {
        static void *clone(const void *p) { return new T(*static_cast<const T *>(p)); }
        static void destroy(void *p) { delete static_cast<T *>(p); }

        static const VTableBase &get()
        {
            static const VTableBase vt{ &clone, &destroy };
            return vt;
        }
    };

    void reset() noexcept
    {
        if (ptr_)
            vtable_->destroy(ptr_);
        ptr_    = nullptr;
        id_     = nullptr;
        vtable_ = nullptr;
    }

    void *ptr_ = nullptr;
    const void *id_ = nullptr;
    const VTableBase *vtable_ = nullptr;
};

} // namespace Core

#endif // REFLECT_REFLECT_VALUE_H
