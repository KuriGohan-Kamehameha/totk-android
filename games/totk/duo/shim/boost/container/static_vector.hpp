// SPDX-License-Identifier: GPL-3.0-or-later
// Port shim, not Boost: the one container the vendored ASTC decoder takes from Boost, so the port needs no Boost
// tree. A fixed-capacity vector for trivially destructible elements; the option types only name what the decoder
// asks for (in-place alignment, no throw on overflow) and change nothing here: an append past the capacity is
// dropped, which is what throw_on_overflow<false> leaves undefined in Boost.
#pragma once
#include <cstddef>
#include <type_traits>
#include <utility>

namespace boost::container {
template <std::size_t> struct inplace_alignment {};
template <bool> struct throw_on_overflow {};
template <typename...> struct static_vector_options { struct type {}; };

template <typename T, std::size_t Capacity, typename Options = void>
class static_vector {
    static_assert(std::is_trivially_destructible_v<T>);

public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;
    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if (count == Capacity) {
            return items[Capacity - 1];
        }
        items[count] = T(std::forward<Args>(args)...);
        return items[count++];
    }
    void push_back(const T& value) { emplace_back(value); }
    [[nodiscard]] std::size_t size() const { return count; }
    [[nodiscard]] bool empty() const { return count == 0; }
    void clear() { count = 0; }
    T& operator[](std::size_t i) { return items[i]; }
    const T& operator[](std::size_t i) const { return items[i]; }
    T& back() { return items[count - 1]; }
    iterator begin() { return items; }
    iterator end() { return items + count; }
    const_iterator begin() const { return items; }
    const_iterator end() const { return items + count; }

private:
    T items[Capacity]{};
    std::size_t count{};
};
} // namespace boost::container
