//
// Copyright (c) 2022 ZettaScale Technology
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Apache License, Version 2.0
// which is available at https://www.apache.org/licenses/LICENSE-2.0.
//
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
//
// Contributors:
//   ZettaScale Zenoh Team, <zenoh@zettascale.tech>

#pragma once

#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "assert.h"
#include "string.h"

namespace zenohcxx {

//
// Template base classes implementing common functionality
//

//
// Base type for C++ wrappers of Zenoh copyable structures, like GetOptions, PutOptions, etc.
//
template <typename ZC_COPYABLE_TYPE>
struct Copyable : public ZC_COPYABLE_TYPE {
    Copyable() = delete;  // May be overloaded in derived structs with corresponding z_XXX_default function
    Copyable(const Copyable& v) { *this = v; }
    Copyable(ZC_COPYABLE_TYPE v) : ZC_COPYABLE_TYPE(v) {}
};

//
// Base type for C++ wrappers of Zenoh owned structures, copying not allowed
//
template <typename ZC_OWNED_TYPE>
class Owned {
   public:
    // Default constructor is deleted by default, derived classes may override it to create default valid object.
    // It's supposed that default constructor never creates null object, this should be done explicitly with constructor
    // from nullptr
    Owned() = delete;
    // Copying is not allowed, owned object have ownership of it's value
    Owned& operator=(const Owned& v) = delete;
    Owned(const Owned& v) = delete;
    // Creating from pointer to value is allowed, ownership is taken and value is made null
    // Also explicit creation of null owned object is allowed if nullptr is passed
    Owned(ZC_OWNED_TYPE* pv) {
        if (pv) {
            _0 = *pv;
            ::z_null(*pv);
        } else
            ::z_null(_0);
    }
    // Move constructor from wrapped value
    Owned(ZC_OWNED_TYPE&& v) : _0(v) { ::z_null(v); }
    // Move constructor from other object
    Owned(Owned&& v) : Owned(std::move(v._0)) {}
    // Move assignment from other object
    Owned&& operator=(Owned&& v) {
        if (this != &v) {
            drop();
            _0 = v._0;
            ::z_null(v._0);
        }
        return std::move(*this);
    }
    // Destructor drops owned value using z_drop from zenoh API
    ~Owned() { ::z_drop(&_0); }
    // Explicit drop. Making value null is zenoh API job
    void drop() { ::z_drop(&_0); }
    // Take zenoh structure and leave Owned object null
    ZC_OWNED_TYPE take() {
        auto r = _0;
        ::z_null(_0);
        return r;
    }
    // Replace value with zenoh structure, dropping old value
    void put(ZC_OWNED_TYPE& v) {
        ::z_drop(&_0);
        _0 = v;
        ::z_null(v);
    }
    // Get direct access to wrapped zenoh structure
    explicit operator ZC_OWNED_TYPE&() { return _0; }
    explicit operator const ZC_OWNED_TYPE&() const { return _0; }
    // Check object validity uzing zenoh API
    bool check() const { return ::z_check(_0); }

   protected:
    ZC_OWNED_TYPE _0;
};

//
// Base type for C++ wrappers for Zenoh closures which doesn't take ownership of passed value
// It expects that
// - ZCPP_PARAM type is derived from ZC_PARAM
//
// All zenoh types wrapped with 'ClosureConstPtrParam' are defined this way:
//
// typedef struct ZC_CLOSURE_TYPE {
//   void *context;
//   void (*call)(const struct ZC_PARAM*, void*);
//   void (*drop)(void*);
// } ZC_CLOSURE_TYPE;
//
// `ClosureConstPtrParam` can be constructed from the following objects:
//
// - function pointer of type `void (func*)(const ZCPP_PARAM&)`
//
//   Example:
//
//   void on_query(const Query&) { ... }
//   ...
//   session.declare_queryable("foo/bar", on_query);
//
// - any object which can be called with `operator()(const ZCPP_PARAM&)`, e.g. lambda,
//   passed my move reference. In this case `ClosureConstPtrParam` will take ownership
//   of the object and will call it with `operator()(const ZCPP_PARAM&)` when `call` is called
//   and will drop it when `drop` is called.
//
//   Example:
//
//   session.declare_queryable("foo/bar", [](const Query&) { ... });
//
//   or
//
//   struct OnQuery {
//     void operator()(const Query&) { ... }
//     ~OnQuery() { ... }
//   };
//
//   OnQuery on_query;
//   session.declare_queryable("foo/bar", std::move(on_query));
//
template <typename ZC_CLOSURE_TYPE, typename ZC_PARAM, typename ZCPP_PARAM,
          typename std::enable_if_t<std::is_base_of_v<ZC_PARAM, ZCPP_PARAM> && sizeof(ZC_PARAM) == sizeof(ZCPP_PARAM),
                                    bool> = true>
class ClosureConstRefParam : public Owned<ZC_CLOSURE_TYPE> {
   public:
    using Owned<ZC_CLOSURE_TYPE>::Owned;

    // Closure is valid if it can be called. The drop operation is optional
    bool check() const { return Owned<ZC_CLOSURE_TYPE>::_0.call != nullptr; }

    // Call closure with pointer to C parameter
    void call(ZC_PARAM* v) {
        if (check()) _0.call(v, _0.context);
    }

    // Call closure with reference to C++ parameter
    void operator()(const ZCPP_PARAM& v) { return call(&(static_cast<ZC_PARAM&>(v))); }

    // Construct empty closure
    ClosureConstRefParam() : Owned<ZC_CLOSURE_TYPE>(nullptr) {}

    // Construct closure from the data handler: any object with operator()(ZCPP_PARAM&&) defined
    template <typename T>
    ClosureMoveParam(T&& obj) : Owned<ZC_CLOSURE_TYPE>(wrap_call(std::forward<T>(obj), nullptr)) {}

    // Add data handler
    template <typename T>
    ClosureMoveParam& add_call(T&& obj) {
        _0 = wrap_call(std::forward(obj), &_0);
        return *this;
    }

    // Add drop handler
    template <typename T>
    ClosureMoveParam& add_drop(T&& obj) {
        _0 = wrap_drop(std::forward(obj), &_0);
        return *this;
    }

   private:
    template <typename T>
    ZC_CLOSURE_TYPE wrap_call(T& obj, ZC_CLOSURE_TYPE* prev) {
        auto context = new std::pair{&obj, ClosureMoveParam(prev)};
        auto call = [](ZC_PARAM* pvalue, void* ctx) -> ZC_RETVAL {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            ZCPP_PARAM param(pvalue);
            (*pair->first)(std::move(param));
            return pair->second(std::move(param));
        };
        auto drop = [](void* ctx) {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            delete pair;
        };
        return {context, call, drop};
    }

    template <typename T>
    ZC_CLOSURE_TYPE wrap_call(T&& obj, ZC_CLOSURE_TYPE* prev) {
        auto context = new std::pair{new T(std::move(obj)), ClosureMoveParam(prev)};
        auto call = [](ZC_PARAM* pvalue, void* ctx) -> ZC_RETVAL {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            ZCPP_PARAM param(pvalue);
            (*pair->first)(std::move(param));
            return pair->second(std::move(param));
        };
        auto drop = [](void* ctx) {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            delete pair->first;
            delete pair;
        };
        return {context, call, drop};
    }

    template <typename T>
    ZC_CLOSURE_TYPE wrap_drop(T& obj, ZC_CLOSURE_TYPE* prev) {
        auto context = new std::pair{&obj, ClosureMoveParam(prev)};
        auto call = [](ZC_PARAM* pvalue, void* ctx) -> ZC_RETVAL {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            return pair->second.call(pvalue);
        };
        auto drop = [](void* ctx) {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            (*pair->first)();
            delete pair;
        };
        return {context, call, drop};
    }

    template <typename T>
    ZC_CLOSURE_TYPE wrap_drop(T&& obj, ZC_CLOSURE_TYPE* prev) {
        auto context = new std::pair{new T(std::move(obj)), ClosureMoveParam(prev)};
        auto call = [](ZC_PARAM* pvalue, void* ctx) -> ZC_RETVAL {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            return pair->second.call(pvalue);
        };
        auto drop = [](void* ctx) {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            (*pair->first)();
            delete pair->first;
            delete pair;
        };
        return {context, call, drop};
    }
};

//
// Base type for C++ wrappers for Zenoh closures which takes ownership of passed value
// It expects that
// - ZCPP_PARAM is derived from Owned<ZC_PARAM>
// - user's LAMBDA can be invoked with ZCPP_PARAM&&
//
template <typename ZC_CLOSURE_TYPE, typename ZC_PARAM, typename ZCPP_PARAM,
          typename std::enable_if_t<std::is_base_of_v<Owned<ZC_PARAM>, ZCPP_PARAM>, bool> = true>

class ClosureMoveParam : public Owned<ZC_CLOSURE_TYPE> {
    // The `z_owned_reply_channel_closure_t::call` have the return type `bool` instead of void
    // So have to use `decltype` to get the return type of the closure
    typedef decltype((*ZC_CLOSURE_TYPE::call)(nullptr, nullptr)) ZC_RETVAL;

   public:
    using Owned<ZC_CLOSURE_TYPE>::Owned;

    // Closure is valid if it can be called. The drop operation is optional
    bool check() const { return Owned<ZC_CLOSURE_TYPE>::_0.call != nullptr; }

    // Call closure with pointer to C parameter
    ZC_RETVAL call(ZC_PARAM* v) { return check() ? _0.call(v, Owned<ZC_CLOSURE_TYPE>::_0.context) : ZC_RETVAL{}; }

    // Call closure with reference to C++ parameter
    ZC_RETVAL operator()(ZCPP_PARAM&& v) { return call(&(static_cast<ZC_PARAM&>(v))); }

    // Construct empty closure
    ClosureMoveParam() : Owned<ZC_CLOSURE_TYPE>(nullptr) {}

    // Construct closure from the data handler: any object with operator()(ZCPP_PARAM&&) defined
    template <typename T>
    ClosureMoveParam(T&& obj) : Owned<ZC_CLOSURE_TYPE>(wrap_call(std::forward<T>(obj), nullptr)) {}

    // Add data handler
    template <typename T>
    ClosureMoveParam& add_call(T&& obj) {
        _0 = wrap_call(std::forward(obj), &_0);
        return *this;
    }

    // Add drop handler
    template <typename T>
    ClosureMoveParam& add_drop(T&& obj) {
        _0 = wrap_drop(std::forward(obj), &_0);
        return *this;
    }

   private:
    template <typename T>
    ZC_CLOSURE_TYPE wrap_call(T& obj, ZC_CLOSURE_TYPE* prev) {
        auto context = new std::pair{&obj, ClosureMoveParam(prev)};
        auto call = [](ZC_PARAM* pvalue, void* ctx) -> ZC_RETVAL {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            ZCPP_PARAM param(pvalue);
            (*pair->first)(std::move(param));
            return pair->second(std::move(param));
        };
        auto drop = [](void* ctx) {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            delete pair;
        };
        return {context, call, drop};
    }

    template <typename T>
    ZC_CLOSURE_TYPE wrap_call(T&& obj, ZC_CLOSURE_TYPE* prev) {
        auto context = new std::pair{new T(std::move(obj)), ClosureMoveParam(prev)};
        auto call = [](ZC_PARAM* pvalue, void* ctx) -> ZC_RETVAL {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            ZCPP_PARAM param(pvalue);
            (*pair->first)(std::move(param));
            return pair->second(std::move(param));
        };
        auto drop = [](void* ctx) {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            delete pair->first;
            delete pair;
        };
        return {context, call, drop};
    }

    template <typename T>
    ZC_CLOSURE_TYPE wrap_drop(T& obj, ZC_CLOSURE_TYPE* prev) {
        auto context = new std::pair{&obj, ClosureMoveParam(prev)};
        auto call = [](ZC_PARAM* pvalue, void* ctx) -> ZC_RETVAL {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            return pair->second.call(pvalue);
        };
        auto drop = [](void* ctx) {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            (*pair->first)();
            delete pair;
        };
        return {context, call, drop};
    }

    template <typename T>
    ZC_CLOSURE_TYPE wrap_drop(T&& obj, ZC_CLOSURE_TYPE* prev) {
        auto context = new std::pair{new T(std::move(obj)), ClosureMoveParam(prev)};
        auto call = [](ZC_PARAM* pvalue, void* ctx) -> ZC_RETVAL {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            return pair->second.call(pvalue);
        };
        auto drop = [](void* ctx) {
            auto pair = static_cast<std::pair<T*, ClosureMoveParam>*>(ctx);
            (*pair->first)();
            delete pair->first;
            delete pair;
        };
        return {context, call, drop};
    }
};

}  // namespace zenohcxx
