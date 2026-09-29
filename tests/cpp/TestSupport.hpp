// Shared helpers for the mikai_tests suite.
//
// The domain structs carry "unknown" as a real state (an anime with no planned
// episode count, a score nobody rated, a poster nobody published), so several
// fields are optional in C++ while the Python dataclasses used `| None`. These
// adapters read a field either way, so a test asserts the value and not the
// spelling of the nullability.
//
// Two traps this header exists to avoid, both hit while writing the suite:
//
//  * There is no `std::is_optional` in C++ -- it is a libstdc++ extension this
//    toolchain does not ship, and `std::optional` itself is all the standard
//    gives you. `IsOptional` below detects it structurally.
//  * Dereferencing a `std::optional<QString>` with `*value` pulls the global
//    namespace into ADL, and Qt overloads `operator*` there for a dozen
//    geometry types, which turns an ordinary dereference into an overload
//    resolution error. Every branch below uses `value()` instead.

#pragma once

#include <optional>
#include <string>
#include <type_traits>

#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QVariant>

namespace mikai_test {

template <typename T>
struct IsOptional : std::false_type {};
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};
template <typename T>
inline constexpr bool isOptional = IsOptional<std::decay_t<T>>::value;

// A QString-like field: QString, std::optional<QString>, QVariant or a C string.
template <typename T>
inline QString asText(const T& value) {
  using D = std::decay_t<T>;
  if constexpr (isOptional<D>) {
    return value.has_value() ? asText(value.value()) : QString{};
  } else if constexpr (std::is_same_v<D, QVariant>) {
    return value.toString();
  } else if constexpr (std::is_same_v<D, QString>) {
    return value;
  } else {
    return QString::fromStdString(std::string(value));
  }
}

// True when an optional text field actually carries a value.
template <typename T>
inline bool asPresent(const T& value) {
  using D = std::decay_t<T>;
  if constexpr (isOptional<D>) {
    return value.has_value();
  } else if constexpr (std::is_same_v<D, QVariant>) {
    return value.isValid() && !value.toString().isEmpty();
  } else {
    return !asText(value).isEmpty();
  }
}

// An integer-like field, missing values reading as 0.
template <typename T>
inline int asInt(const T& value) {
  using D = std::decay_t<T>;
  if constexpr (isOptional<D>) {
    return value.has_value() ? asInt(value.value()) : 0;
  } else if constexpr (std::is_same_v<D, QVariant>) {
    return value.isValid() ? value.toInt() : 0;
  } else if constexpr (std::is_convertible_v<D, double>) {
    return static_cast<int>(value);
  } else {
    return 0;
  }
}

// True when an optional numeric field carries a value.
template <typename T>
inline bool asIntPresent(const T& value) {
  using D = std::decay_t<T>;
  if constexpr (isOptional<D>) {
    return value.has_value();
  } else if constexpr (std::is_same_v<D, QVariant>) {
    return value.isValid();
  } else {
    return asInt(value) != 0;
  }
}

// A double-like field, missing values reading as 0.
template <typename T>
inline double asScore(const T& value) {
  using D = std::decay_t<T>;
  if constexpr (isOptional<D>) {
    return value.has_value() ? asScore(value.value()) : 0.0;
  } else if constexpr (std::is_same_v<D, QVariant>) {
    return value.toDouble();
  } else if constexpr (std::is_convertible_v<D, double>) {
    return static_cast<double>(value);
  } else {
    return 0.0;
  }
}

// A bool-like field.
template <typename T>
inline bool asFlag(const T& value) {
  using D = std::decay_t<T>;
  if constexpr (isOptional<D>) {
    return value.has_value() ? asFlag(value.value()) : false;
  } else if constexpr (std::is_same_v<D, QVariant>) {
    return value.toBool();
  } else if constexpr (std::is_same_v<D, int>) {
    return value != 0;
  } else {
    return static_cast<bool>(value);
  }
}

// The episode number on the wire may arrive as an int, a double ("12.5" for a
// special) or a free-form string ("OVA"); all three read back as their text.
template <typename T>
inline QString asEpisodeText(const T& value) {
  using D = std::decay_t<T>;
  if constexpr (isOptional<D>) {
    return value.has_value() ? asEpisodeText(value.value()) : QString{};
  } else if constexpr (std::is_same_v<D, QVariant>) {
    if (!value.isValid()) {
      return QString{};
    }
    if (value.typeId() == QMetaType::QString) {
      return value.toString();
    }
    return QString::number(value.toDouble(), 'g', 15);
  } else if constexpr (std::is_same_v<D, double>) {
    return QString::number(value, 'g', 15);
  } else if constexpr (std::is_same_v<D, int>) {
    return QString::number(value);
  } else {
    return asText(value);
  }
}

}  // namespace mikai_test
