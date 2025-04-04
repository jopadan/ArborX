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

#include <ArborX_Box.hpp>
#include <ArborX_Point.hpp>
#include <detail/ArborX_AccessTraits.hpp>
#include <detail/ArborX_Callbacks.hpp>
#include <detail/ArborX_Predicates.hpp>

// NOTE Let's not bother with __host__ __device__ annotations here

struct NearestPredicates
{};
template <>
struct ArborX::AccessTraits<NearestPredicates>
{
  using memory_space = Kokkos::HostSpace;
  static int size(NearestPredicates const &) { return 1; }
  static auto get(NearestPredicates const &, int)
  {
    return nearest(Point<3>{});
  }
};

struct SpatialPredicates
{};
template <>
struct ArborX::AccessTraits<SpatialPredicates>
{
  using memory_space = Kokkos::HostSpace;
  static int size(SpatialPredicates const &) { return 1; }
  static auto get(SpatialPredicates const &, int)
  {
    return intersects(Point<3>{});
  }
};

// Custom callbacks
struct CallbackMissingTag
{
  template <typename Predicate, typename OutputFunctor>
  void operator()(Predicate const &, int, OutputFunctor const &) const
  {}
};

struct Wrong
{};

struct CallbackDoesNotTakeCorrectArgument
{
  template <typename OutputFunctor>
  void operator()(Wrong, int, OutputFunctor const &) const
  {}
};

struct CustomCallback
{
  template <class Predicate>
  KOKKOS_FUNCTION void operator()(Predicate const &, int) const
  {}
};

struct CustomCallbackMissingConstQualifier
{
  template <class Predicate>
  KOKKOS_FUNCTION void operator()(Predicate const &, int)
  {}
};

struct CustomCallbackNonVoidReturnType
{
  template <class Predicate>
  KOKKOS_FUNCTION auto operator()(Predicate const &, int) const
  {
    return Wrong{};
  }
};

struct LegacyNearestPredicateCallback
{
  template <class Predicate, class OutputFunctor>
  void operator()(Predicate const &, int, float, OutputFunctor const &) const
  {}
};

void test_callbacks_compile_only()
{
  using ArborX::Details::check_valid_callback;

  // view type does not matter as long as we do not call the output functor
  Kokkos::View<float *> v;

  check_valid_callback<int>(ArborX::Details::DefaultCallback{},
                            SpatialPredicates{}, v);
  check_valid_callback<int>(ArborX::Details::DefaultCallback{},
                            NearestPredicates{}, v);

  // not required to tag inline callbacks anymore
  check_valid_callback<int>(CallbackMissingTag{}, SpatialPredicates{}, v);
  check_valid_callback<int>(CallbackMissingTag{}, NearestPredicates{}, v);

  check_valid_callback<int>(CustomCallback{}, SpatialPredicates{});
  check_valid_callback<int>(CustomCallback{}, NearestPredicates{});

  // generic lambdas are supported if not using NVCC
#ifndef __NVCC__
  check_valid_callback<int>([](auto const & /*predicate*/, int /*primitive*/,
                               auto const & /*out*/) {},
                            SpatialPredicates{}, v);

  check_valid_callback<int>([](auto const & /*predicate*/, int /*primitive*/,
                               auto const & /*out*/) {},
                            NearestPredicates{}, v);

  check_valid_callback<int>(
      [](auto const & /*predicate*/, int /*primitive*/) {},
      SpatialPredicates{});

  check_valid_callback<int>(
      [](auto const & /*predicate*/, int /*primitive*/) {},
      NearestPredicates{});
#endif

  // Uncomment to see error messages

  // check_valid_callback<int>(LegacyNearestPredicateCallback{},
  // NearestPredicates{},
  //                      v);

  // check_valid_callback<int>(CallbackDoesNotTakeCorrectArgument{},
  //                      SpatialPredicates{}, v);

  // check_valid_callback<int>(CustomCallbackNonVoidReturnType{},
  //                           SpatialPredicates{});

  // check_valid_callback<int>(CustomCallbackMissingConstQualifier{},
  //                           SpatialPredicates{});

#ifndef __NVCC__
  // check_valid_callback<int>([](Wrong, int /*primitive*/) {},
  //                           SpatialPredicates{});
#endif
}
