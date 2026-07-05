// RUN: %clang_cc1 %s -std=c++23 -freflection -verify

using info = decltype(^^int);

template <typename T1, typename T2>
constexpr bool is_same_v = false;

template <typename T1>
constexpr bool is_same_v<T1, T1> = true;

template <typename T>
struct Box {
  T value;
};

template <typename T>
Box(T) -> Box<T>;

template <typename Ty>
consteval auto extract(info r) -> Ty {
  return __metafunction(24, ^^Ty, r);
}

template <info R>
using InfoType = [:R:];

namespace valid_ctad {
constexpr [:^^Box:] box = {1};
static_assert(is_same_v<decltype(box), const Box<int>>);
} // namespace valid_ctad

namespace invalid_type_id {
Box<int> object{42};

template <info R>
consteval auto f(info r) -> InfoType<R> {
  // expected-note@-1 {{use of class template 'Box' requires template arguments}}
  return extract<InfoType<R>>(r);
}

static_assert(f<^^Box>(^^object).value == 42);
// expected-error@-1 {{no matching function for call to 'f'}}
} // namespace invalid_type_id
