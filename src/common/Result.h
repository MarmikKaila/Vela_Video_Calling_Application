#pragma once

// Result<T, E> — explicit, exception-free error propagation.
//
// This is the project-wide error-handling primitive. Per the coding standard,
// hot paths must not throw; functions that can fail return a Result and the
// caller is forced (via [[nodiscard]]) to inspect it.
//
// Usage:
//   Result<Frame, Error> grab();
//   auto r = grab();
//   if (!r) return fail(r.error());   // propagate
//   use(r.value());
//
// For functions that return only success/failure (no value), use Status
// (see the alias at the bottom of this file).

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

namespace vc {

// Tag wrapper so an error value can be returned without ambiguity even when
// it shares a type with the success value. Construct via fail(...).
template <typename E>
struct Failure {
    E error;
};

template <typename E>
[[nodiscard]] constexpr Failure<std::decay_t<E>> fail(E&& e) {
    return Failure<std::decay_t<E>>{std::forward<E>(e)};
}

// Empty success payload for Result<Unit, E> (a.k.a. Status).
struct Unit {};
inline constexpr Unit unit{};

template <typename T, typename E>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T,E> cannot hold a reference");

public:
    using value_type = T;
    using error_type = E;

    // Implicit construction from a success value: `return frame;`
    Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}

    // Implicit construction from `return fail(err);`
    Result(Failure<E> f) : data_(std::in_place_index<1>, std::move(f.error)) {}

    [[nodiscard]] bool has_value() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        assert(has_value() && "Result::value() on an error");
        return std::get<0>(data_);
    }
    const T& value() const& {
        assert(has_value() && "Result::value() on an error");
        return std::get<0>(data_);
    }
    T&& value() && {
        assert(has_value() && "Result::value() on an error");
        return std::get<0>(std::move(data_));
    }

    const E& error() const& {
        assert(!has_value() && "Result::error() on a value");
        return std::get<1>(data_);
    }
    E&& error() && {
        assert(!has_value() && "Result::error() on a value");
        return std::get<1>(std::move(data_));
    }

    template <typename U>
    T value_or(U&& fallback) const& {
        return has_value() ? value() : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, E> data_;
};

} // namespace vc
