#pragma once

#include "core/error.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace core {

template <typename T>
class Result {
public:
    static_assert(!std::is_same_v<T, void>, "Use Result<void> specialization for void");
    static_assert(!std::is_same_v<T, Error>, "Result type cannot be Error");

    Result(T value)
        : storage_(std::move(value)) {}

    Result(Error error)
        : storage_(std::move(error)) {}

    Result(const Result&) = default;
    Result& operator=(const Result&) = default;
    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;

    ~Result() = default;

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return has_value();
    }

    [[nodiscard]] bool has_error() const noexcept {
        return std::holds_alternative<Error>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] T& value() & {
        ensure_value();
        return std::get<T>(storage_);
    }

    [[nodiscard]] const T& value() const & {
        ensure_value();
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() && {
        ensure_value();
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] const T&& value() const && {
        ensure_value();
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] T* operator->() noexcept {
        return &std::get<T>(storage_);
    }

    [[nodiscard]] const T* operator->() const noexcept {
        return &std::get<T>(storage_);
    }

    [[nodiscard]] T& operator*() & noexcept {
        return std::get<T>(storage_);
    }

    [[nodiscard]] const T& operator*() const & noexcept {
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& operator*() && noexcept {
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] const T&& operator*() const && noexcept {
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] const Error& error() const noexcept {
        return std::get<Error>(storage_);
    }

    [[nodiscard]] Error& error() noexcept {
        return std::get<Error>(storage_);
    }

private:
    void ensure_value() const {
        if (!has_value()) {
            throw std::runtime_error("Attempted to access value of failed Result: " + error().to_string());
        }
    }

    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    Result() noexcept
        : error_{} {}

    Result(Error error) noexcept
        : error_(std::move(error)) {}

    [[nodiscard]] static Result success() noexcept {
        return Result{};
    }

    [[nodiscard]] static Result failure(Error error) noexcept {
        return Result{std::move(error)};
    }

    [[nodiscard]] bool has_value() const noexcept {
        return !error_.is_error();
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return has_value();
    }

    [[nodiscard]] bool has_error() const noexcept {
        return error_.is_error();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] const Error& error() const noexcept {
        return error_;
    }

    [[nodiscard]] Error& error() noexcept {
        return error_;
    }

    void value() const {
        if (has_error()) {
            throw std::runtime_error("Attempted to access value of failed Result<void>: " + error_.to_string());
        }
    }

private:
    Error error_;
};

} // namespace core
