// RUN: %clang_cc1 -std=c++2c -fexpansion-statements -x c++-header %s -emit-pch -o %t.pch
// RUN: wc -c %t.pch | FileCheck --check-prefix=SIZE %s
// RUN: %clang_cc1 -std=c++2c -fexpansion-statements -x c++ %s -include-pch %t.pch -fsyntax-only

// SIZE-NOT: {{^[ ]*[1-9][0-9]{9,} }}
// SIZE: {{^[ ]*[0-9]+ }}

#ifndef HEADER_INCLUDED
#define HEADER_INCLUDED

template<int N>
struct TinyArray {
  char data[N == 0 ? 1 : N];
};

template<int... Vs>
struct Range {
  int values[sizeof...(Vs)] = {Vs...};
};

template<int... Vs>
constexpr const int *begin(const Range<Vs...> &R) {
  return &R.values[0];
}

template<int... Vs>
constexpr const int *end(const Range<Vs...> &R) {
  return &R.values[sizeof...(Vs)];
}

template<typename T>
inline constexpr T DependentRange{};

template<typename T>
consteval int dependent_expansion_with_dependent_local_type() {
  int Result = 0;
  template for (constexpr auto Element : DependentRange<T>) {
    constexpr int Name = Element;
    constexpr auto Bytes = [] {
      TinyArray<Name> Out{};
      return Out;
    }();
    Result += sizeof(Bytes.data);
  }
  return Result;
}

#else

static_assert(dependent_expansion_with_dependent_local_type<Range<1, 2>>() == 3);

#endif