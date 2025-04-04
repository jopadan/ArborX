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

#ifndef ARBORX_MINIMUM_SPANNING_TREE_HPP
#define ARBORX_MINIMUM_SPANNING_TREE_HPP

#include <ArborX_LinearBVH.hpp>
#include <detail/ArborX_AccessTraits.hpp>
#include <detail/ArborX_BoruvkaHelpers.hpp>
#include <detail/ArborX_MutualReachabilityDistance.hpp>
#include <detail/ArborX_PredicateHelpers.hpp>
#include <detail/ArborX_TreeNodeLabeling.hpp>
#include <detail/ArborX_WeightedEdge.hpp>
#include <kokkos_ext/ArborX_KokkosExtStdAlgorithms.hpp>
#include <kokkos_ext/ArborX_KokkosExtViewHelpers.hpp>

#include <Kokkos_Core.hpp>
#include <Kokkos_Profiling_ScopedRegion.hpp>

namespace ArborX::Experimental
{

template <class MemorySpace,
          Details::BoruvkaMode Mode = Details::BoruvkaMode::MST>
struct MinimumSpanningTree
{
  using memory_space = MemorySpace;
  static_assert(Kokkos::is_memory_space<MemorySpace>::value);

  Kokkos::View<WeightedEdge *, MemorySpace> edges;
  Kokkos::View<int *, MemorySpace> dendrogram_parents;
  Kokkos::View<float *, MemorySpace> dendrogram_parent_heights;
  Kokkos::View<int *, MemorySpace> _chain_offsets;
  Kokkos::View<int *, MemorySpace> _chain_levels;

  template <class ExecutionSpace, class Primitives>
  MinimumSpanningTree(ExecutionSpace const &space, Primitives const &primitives,
                      int k = 1)
      : edges(Kokkos::view_alloc(space, Kokkos::WithoutInitializing,
                                 "ArborX::MST::edges"),
              AccessTraits<Primitives>::size(primitives) - 1)
      , dendrogram_parents("ArborX::MST::dendrogram_parents", 0)
      , dendrogram_parent_heights("ArborX::MST::dendrogram_parent_heights", 0)
      , _chain_offsets("ArborX::MST::chain_offsets", 0)
      , _chain_levels("ArborX::MST::chain_levels", 0)
  {
    Kokkos::Profiling::pushRegion("ArborX::MST::MST");

    using Points = Details::AccessValues<Primitives>;
    using Point = typename Points::value_type;
    static_assert(GeometryTraits::is_point_v<Point>);

    Points points{primitives}; // NOLINT

    auto const n = points.size();

    Kokkos::Profiling::pushRegion("ArborX::MST::construction");
    BoundingVolumeHierarchy bvh(space, Experimental::attach_indices(points));
    Kokkos::Profiling::popRegion();

    if (k > 1)
    {
      Kokkos::Profiling::pushRegion("ArborX::MST::compute_core_distances");
      Kokkos::View<float *, MemorySpace> core_distances(
          "ArborX::MST::core_distances", n);
      bvh.query(
          space,
          Experimental::attach_indices(Experimental::make_nearest(points, k)),
          Details::MaxDistance<Points, decltype(core_distances)>{
              points, core_distances});
      Kokkos::Profiling::popRegion();

      Details::MutualReachability<decltype(core_distances)> mutual_reachability{
          core_distances};
      Kokkos::Profiling::pushRegion("ArborX::MST::boruvka");
      doBoruvka(space, bvh, mutual_reachability);
      Kokkos::Profiling::popRegion();
    }
    else
    {
      Kokkos::Profiling::pushRegion("ArborX::MST::boruvka");
      doBoruvka(space, bvh, Details::Euclidean{});
      Kokkos::Profiling::popRegion();
    }

    Details::finalizeEdges(space, bvh, edges);

    Kokkos::Profiling::popRegion();
  }

  // enclosing function for an extended __host__ __device__ lambda cannot have
  // private or protected access within its class
#ifndef KOKKOS_COMPILER_NVCC
private:
#endif
  template <class ExecutionSpace, class BVH, class Metric>
  void doBoruvka(ExecutionSpace const &space, BVH const &bvh,
                 Metric const &metric)
  {
    namespace KokkosExt = ArborX::Details::KokkosExt;

    auto const n = bvh.size();
    Kokkos::View<int *, MemorySpace> tree_parents(
        Kokkos::view_alloc(space, Kokkos::WithoutInitializing,
                           "ArborX::MST::tree_parents"),
        2 * n - 1);
    Details::findParents(space, bvh, tree_parents);

    Kokkos::Profiling::pushRegion("ArborX::MST::initialize_node_labels");
    Kokkos::View<int *, MemorySpace> labels(
        Kokkos::view_alloc(space, Kokkos::WithoutInitializing,
                           "ArborX::MST::labels"),
        2 * n - 1);
    KokkosExt::iota(space,
                    Kokkos::subview(labels, std::make_pair((decltype(n))0, n)));
    Kokkos::Profiling::popRegion();

    Kokkos::View<Details::DirectedEdge *, MemorySpace> component_out_edges(
        Kokkos::view_alloc(space, Kokkos::WithoutInitializing,
                           "ArborX::MST::component_out_edges"),
        n);

    Kokkos::View<float *, MemorySpace> weights(
        Kokkos::view_alloc(space, Kokkos::WithoutInitializing,
                           "ArborX::MST::weights"),
        n);

    Kokkos::View<float *, MemorySpace> radii(
        Kokkos::view_alloc(space, Kokkos::WithoutInitializing,
                           "ArborX::MST::radii"),
        n);

    Kokkos::View<float *, MemorySpace> lower_bounds("ArborX::MST::lower_bounds",
                                                    0);

    constexpr bool use_lower_bounds =
#ifdef KOKKOS_ENABLE_SERIAL
        std::is_same<ExecutionSpace, Kokkos::Serial>::value;
#else
        false;
#endif

    // Shared radii may or may not be faster for CUDA depending on the problem.
    // In the ICPP'51 paper experiments, we ended up using it only in Serial.
    // But we would like to keep an option open for the future, so the code is
    // written to be able to run it if we want.
    constexpr bool use_shared_radii =
#ifdef KOKKOS_ENABLE_SERIAL
        std::is_same<ExecutionSpace, Kokkos::Serial>::value;
#else
        false;
#endif

    if constexpr (use_lower_bounds)
    {
      KokkosExt::reallocWithoutInitializing(space, lower_bounds, n);
      Kokkos::deep_copy(space, lower_bounds, 0);
    }

    Kokkos::Profiling::pushRegion("ArborX::MST::Boruvka_loop");
    Kokkos::View<int, MemorySpace> num_edges(
        Kokkos::view_alloc(space, "ArborX::MST::num_edges")); // initialize to 0

    Kokkos::View<int *, MemorySpace> edges_mapping("ArborX::MST::edges_mapping",
                                                   0);

    Kokkos::View<int *, MemorySpace> sided_parents("ArborX::MST::sided_parents",
                                                   0);
    if constexpr (Mode == Details::BoruvkaMode::HDBSCAN)
    {
      KokkosExt::reallocWithoutInitializing(space, edges_mapping, n - 1);
      KokkosExt::reallocWithoutInitializing(space, sided_parents, n - 1);
      KokkosExt::reallocWithoutInitializing(space, dendrogram_parents,
                                            2 * n - 1);
    }

    // Boruvka iterations
    int iterations = 0;
    int num_components = n;
    [[maybe_unused]] int edges_start = 0;
    [[maybe_unused]] int edges_end = 0;
    std::vector<int> edge_offsets;
    edge_offsets.push_back(0);
    do
    {
      Kokkos::Profiling::pushRegion("ArborX::Boruvka_" +
                                    std::to_string(++iterations) + "_" +
                                    std::to_string(num_components));

      // Propagate leaf node labels to internal nodes
      Details::reduceLabels(space, tree_parents, labels);

      constexpr auto inf = KokkosExt::ArithmeticTraits::infinity<float>::value;
      constexpr Details::DirectedEdge uninitialized_edge;
      Kokkos::deep_copy(space, component_out_edges, uninitialized_edge);
      Kokkos::deep_copy(space, weights, inf);
      Kokkos::deep_copy(space, radii, inf);
      resetSharedRadii(space, bvh, labels, metric, radii);

      Details::FindComponentNearestNeighbors(
          space, bvh, labels, weights, component_out_edges, metric, radii,
          lower_bounds, std::bool_constant<use_shared_radii>());
      retrieveEdges(space, labels, weights, component_out_edges);
      if constexpr (use_lower_bounds)
      {
        updateLowerBounds(space, labels, component_out_edges, lower_bounds);
      }

      Details::UpdateComponentsAndEdges<
          decltype(labels), decltype(component_out_edges), decltype(edges),
          decltype(edges_mapping), decltype(num_edges), Mode>
          f{labels, component_out_edges, edges, edges_mapping, num_edges};

      // For every component C and a found shortest edge `(u, w)`, add the
      // edge to the list of MST edges.
      Kokkos::parallel_for(
          "ArborX::MST::update_unidirectional_edges",
          Kokkos::RangePolicy<ExecutionSpace, Details::UnidirectionalEdgesTag>(
              space, 0, n),
          f);

      int num_edges_host;
      Kokkos::deep_copy(space, num_edges_host, num_edges);
      space.fence();

      edge_offsets.push_back(num_edges_host);

      if constexpr (Mode == Details::BoruvkaMode::HDBSCAN)
      {
        Kokkos::parallel_for(
            "ArborX::MST::update_bidirectional_edges",
            Kokkos::RangePolicy<ExecutionSpace, Details::BidirectionalEdgesTag>(
                space, 0, n),
            f);

        if (iterations > 1)
          Details::updateSidedParents(space, labels, edges, edges_mapping,
                                      sided_parents, edges_start, edges_end);
        else
        {
          Kokkos::Profiling::ScopedRegion guard(
              "ArborX::MST::compute_vertex_parents");
          Details::assignVertexParents(space, labels, component_out_edges,
                                       edges_mapping, bvh, dendrogram_parents);
        }
      }

      // For every component C and a found shortest edge `(u, w)`, merge C
      // with the component that w belongs to by updating the labels
      Kokkos::parallel_for(
          "ArborX::MST::update_labels",
          Kokkos::RangePolicy<ExecutionSpace, Details::LabelsTag>(space, 0, n),
          f);

      num_components = static_cast<int>(n) - num_edges_host;

      edges_start = edges_end;
      edges_end = num_edges_host;

      Kokkos::Profiling::popRegion();
    } while (num_components > 1);

    // Deallocate some memory to reduce high water mark
    Kokkos::resize(edges_mapping, 0);
    Kokkos::resize(lower_bounds, 0);
    Kokkos::resize(radii, 0);
    Kokkos::resize(labels, 0);
    Kokkos::resize(weights, 0);
    Kokkos::resize(component_out_edges, 0);
    Kokkos::resize(tree_parents, 0);

    if constexpr (Mode == Details::BoruvkaMode::HDBSCAN)
    {

      // Done with the recursion as there are no more alpha edges. Assign
      // all current edges to the root chain.
      Kokkos::deep_copy(space,
                        Kokkos::subview(sided_parents,
                                        std::make_pair(edges_start, edges_end)),
                        Details::ROOT_CHAIN_VALUE);

      Kokkos::View<int *, MemorySpace> edge_hierarchy_offsets(
          Kokkos::view_alloc(space, Kokkos::WithoutInitializing,
                             "ArborX::MST::edge_hierarchy_offsets"),
          edge_offsets.size());
      Kokkos::deep_copy(
          space, edge_hierarchy_offsets,
          Kokkos::View<int *, Kokkos::HostSpace, Kokkos::MemoryUnmanaged>{
              edge_offsets.data(), edge_offsets.size()});

      Details::computeParentsAndReorderEdges(
          space, edges, edge_hierarchy_offsets, sided_parents,
          dendrogram_parents, _chain_offsets, _chain_levels);
      Kokkos::resize(sided_parents, 0);

      KokkosExt::reallocWithoutInitializing(space, dendrogram_parent_heights,
                                            n - 1);
      Kokkos::parallel_for(
          "ArborX::MST::assign_dendrogram_parent_heights",
          Kokkos::RangePolicy(space, 0, n - 1),
          KOKKOS_CLASS_LAMBDA(int const e) {
            dendrogram_parent_heights(e) = edges(e).weight;
          });
    }

    Kokkos::Profiling::popRegion();
  }
};

} // namespace ArborX::Experimental

#endif
