/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 and only version 2.1 as published by the Free Software Foundation
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */
#ifndef SRC_MODULES_GROUP_MODULES_MODULES_ENUMS_H_
#define SRC_MODULES_GROUP_MODULES_MODULES_ENUMS_H_

#include <type_traits>

/** Type trait for enums that can be used as flags.
 *
 * By default, the compiler doesn't allow bitwise operations on enums without
 * using cast. By specializing is_flags to be a true_type for a given enum,
 * those operation gets define.
 *
 * Because using a value outside the range of the enum(*) result in a undefined
 * value (or undefined behavior depending on the version of C++), kAll, which
 * is the combination of all flags, is expected to be defined for operations
 * that need a mask, notably '~'.
 *
 * (*) a range is defined as the smallest bitfield needed to encode all the
 * values. For instance, for "enum{A=1, B=4}", the range is [0..7].
 *
 * Example:
 * @code{.cc}
 * enum class Foo {
 *   kOne = 1,
 *   kTwo = 2,
 *
 *   kAll = kOne | kTwo
 * };
 * template <> struct is_flags<Foo> : std::true_type {};
 * [...]
 * Foo foo = Foo::kOne | Foo::kTwo;
 * @endcode
 */
template <typename E, typename = typename std::enable_if<std::is_enum<E>::value>::type>
struct is_flags : std::false_type {};

template <typename E, typename = typename std::enable_if<is_flags<E>::value>::type>
constexpr E operator~(E rhs) {
    return static_cast<E>(
        E::kAll
        & ~static_cast<typename std::underlying_type<E>::type>(rhs));
}

template <typename E, typename = typename std::enable_if<is_flags<E>::value>::type>
constexpr E operator|(E lhs, E rhs) {
    return static_cast<E>(
        static_cast<typename std::underlying_type<E>::type>(lhs)
        | static_cast<typename std::underlying_type<E>::type>(rhs));
}

template <typename E, typename = typename std::enable_if<is_flags<E>::value>::type>
constexpr E operator&(E lhs, E rhs) {
    return static_cast<E>(static_cast<typename std::underlying_type<E>::type>(lhs)
        & static_cast<typename std::underlying_type<E>::type>(rhs));
}

template <typename E, typename = typename std::enable_if<is_flags<E>::value>::type>
constexpr E operator^(E lhs, E rhs) {
    return static_cast<E>(static_cast<typename std::underlying_type<E>::type>(lhs)
        ^ static_cast<typename std::underlying_type<E>::type>(rhs));
}

template <typename E, typename = typename std::enable_if<is_flags<E>::value>::type>
E& operator|=(E& lhs, E rhs) {  // NOLINT(runtime/references)
    lhs = static_cast<E>(
        static_cast<typename std::underlying_type<E>::type>(lhs)
        | static_cast<typename std::underlying_type<E>::type>(rhs));
    return lhs;
}

template <typename E, typename = typename std::enable_if<is_flags<E>::value>::type>
E& operator&=(E& lhs, E rhs) {  // NOLINT(runtime/references)
    lhs = static_cast<E>(
        static_cast<typename std::underlying_type<E>::type>(lhs)
        & static_cast<typename std::underlying_type<E>::type>(rhs));
    return lhs;
}

template <typename E, typename = typename std::enable_if<is_flags<E>::value>::type>
E& operator^=(E& lhs, E rhs) {  // NOLINT(runtime/references)
    lhs = static_cast<E>(
        static_cast<typename std::underlying_type<E>::type>(lhs)
        ^ static_cast<typename std::underlying_type<E>::type>(rhs));
    return lhs;
}

#endif  // SRC_MODULES_GROUP_MODULES_MODULES_ENUMS_H_
