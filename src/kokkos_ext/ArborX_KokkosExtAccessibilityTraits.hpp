/****************************************************************************
 * Copyright (c) 2025, ArborX authors                                       *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the ArborX library. ArborX is                       *
 * distributed under a BSD 3-clause license. For the licensing terms see    *
 * the LICENSE file in the top-level directory.                             *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

#ifndef ARBORX_KOKKOS_EXT_ACCESSIBILITY_TRAITS_HPP
#define ARBORX_KOKKOS_EXT_ACCESSIBILITY_TRAITS_HPP

#include <Kokkos_Core.hpp>

#include <type_traits>

namespace ArborX::Details::KokkosExt
{

template <typename MemorySpace, typename ExecutionSpace, typename = void>
struct is_accessible_from : std::false_type
{
  static_assert(Kokkos::is_memory_space<MemorySpace>::value);
  static_assert(Kokkos::is_execution_space<ExecutionSpace>::value);
};

template <typename MemorySpace, typename ExecutionSpace>
struct is_accessible_from<MemorySpace, ExecutionSpace,
                          std::enable_if_t<Kokkos::SpaceAccessibility<
                              ExecutionSpace, MemorySpace>::accessible>>
    : std::true_type
{};

template <typename View>
struct is_accessible_from_host
    : public is_accessible_from<typename View::memory_space, Kokkos::HostSpace>
{
  static_assert(Kokkos::is_view<View>::value);
};

} // namespace ArborX::Details::KokkosExt

#endif
