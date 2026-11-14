/*
  The internal error model: a value or a typed error, never both.

  The library and the Handler are C++ inside and C at the edge. A fallible
  internal call returns Result<T, E>; the C entry points in Api.cpp translate a
  failure into SDL_APP_FAILURE plus SDL_GetError, and no exception crosses that
  boundary. There are two error kinds:

    * ProtocolError is a malformed message or tool argument: the peer sent
      something the protocol rejects, and the caller answers with a tool error.
    * TransportError is a stream, dial, or process failure: the channel itself
      did not work, so the caller reports a startup or connection failure.

  Both carry the one-line message the caller would otherwise have written into a
  `std::string& error` out-parameter.
*/

#pragma once

#include <string>
#include <utility>
#include <variant>

namespace sdl3mcp {

struct ProtocolError {
   std::string message;
};

struct TransportError {
   std::string message;
};

/*
  Either a T or an E. `hasValue()` (or the explicit bool) says which; `value()`
  and `error()` are only valid on the matching arm, exactly like std::variant.
  The value form has no default constructor, so a success always names its value.
*/
template <typename T, typename E>
class Result {
  public:
   Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {
   }
   Result(E error) : storage_(std::in_place_index<1>, std::move(error)) {
   }

   bool hasValue() const noexcept { return storage_.index() == 0; }
   explicit operator bool() const noexcept { return hasValue(); }

   T& value() & { return std::get<0>(storage_); }
   const T& value() const& { return std::get<0>(storage_); }
   T&& value() && { return std::get<0>(std::move(storage_)); }

   T& operator*() & { return value(); }
   const T& operator*() const& { return value(); }
   T* operator->() { return &value(); }
   const T* operator->() const { return &value(); }

   E& error() & { return std::get<1>(storage_); }
   const E& error() const& { return std::get<1>(storage_); }
   E&& error() && { return std::get<1>(std::move(storage_)); }

  private:
   std::variant<T, E> storage_;
};

/*
  The no-value form: success, or an error. A default-constructed Result is the
  success value, so a void-returning check ends with `return {};`.
*/
template <typename E>
class Result<void, E> {
  public:
   Result() noexcept = default;
   Result(E error) : error_(std::move(error)), hasValue_(false) {
   }

   bool hasValue() const noexcept { return hasValue_; }
   explicit operator bool() const noexcept { return hasValue_; }

   E& error() & { return error_; }
   const E& error() const& { return error_; }
   E&& error() && { return std::move(error_); }

  private:
   E error_{};
   bool hasValue_ = true;
};

}  // namespace sdl3mcp
