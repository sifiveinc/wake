/*
 * Copyright 2026 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>

namespace wcl {

// An efficient, type-erasing, non-owning reference to a callable. This is
// intended for use as the type of a function parameter that is not used
// after the function in question returns.
//
// This class does not own the callable, so it is not in general safe to store
// a function_ref.
//
// Based on LLVM's function_ref and std::function_ref (C++26).
// Compared to LLVM's function_ref, this version is not nullable (like in C++26),
// and has some safety and optimization for function pointers.
template <typename Fn>
class function_ref;

template <typename Ret, typename... Args>
class function_ref<Ret(Args...)> {
 private:
  intptr_t callable_;

  Ret (*trampoline_)(intptr_t, Args...);

  // Trampoline for callable objects (lambdas, functors)
  template <typename Callable>
  static constexpr Ret trampoline_impl(intptr_t callable, Args... args) {
    return (*reinterpret_cast<Callable*>(callable))(std::forward<Args>(args)...);
  }

  // Trampoline for function pointers - callable is the function itself, not pointer-to-pointer
  template <typename F>
  static constexpr Ret function_trampoline_impl(intptr_t fn_ptr, Args... args) {
    return reinterpret_cast<F*>(fn_ptr)(std::forward<Args>(args)...);
  }

 public:
  // Construct from function pointer (stores function directly, not pointer-to-pointer)
  // This overload prevents dangling when passing function pointers through temporaries
  template <typename F, typename = std::enable_if_t<std::is_function_v<F> &&
                                                    std::is_invocable_r_v<Ret, F*, Args...>>>
  constexpr function_ref(F* f) noexcept
      : callable_(reinterpret_cast<intptr_t>(f)), trampoline_(function_trampoline_impl<F>) {
    static_assert(sizeof(f) == sizeof(callable_), "Function pointer size mismatch");
  }

  // Construct from any other compatible callable (lambda, function object, etc.)
  // Disallows use as copy constructor.
  template <typename Callable,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<Callable>, function_ref> &&
                                        !std::is_pointer_v<std::decay_t<Callable>> &&
                                        std::is_invocable_r_v<Ret, Callable, Args...>>>
  constexpr function_ref(Callable&& callable) noexcept
      : callable_(reinterpret_cast<intptr_t>(std::addressof(callable))),
        trampoline_(trampoline_impl<std::remove_reference_t<Callable>>) {}

  constexpr Ret operator()(Args... args) const {
    return trampoline_(callable_, std::forward<Args>(args)...);
  }

  constexpr function_ref(const function_ref&) noexcept = default;
  constexpr function_ref& operator=(const function_ref&) noexcept = default;

  // Prevent assignment from anything other than function_ref (C++26 behavior)
  template <typename T>
  function_ref& operator=(T) = delete;
};

}  // namespace wcl
