#include <gtest/gtest.h>
#include <hpc_algorithm.hpp>
#include <hpc_array_vector.hpp>
#include <hpc_execution.hpp>
#include <hpc_index.hpp>
#include <hpc_macros.hpp>
#include <hpc_range.hpp>
#include <hpc_range_sum.hpp>
#include <hpc_vector.hpp>
#include <Kokkos_Pair.hpp>
#include <Kokkos_View.hpp>
#include <lgr_exodus.hpp>
#include <lgr_input.hpp>
#include <lgr_mesh_indices.hpp>
#include <lgr_state.hpp>
#include <otm_arborx_search_impl.hpp>
#include <otm_search.hpp>
#include <otm_tet2meshless.hpp>
#include <unit_tests/otm_unit_mesh.hpp>

using namespace lgr;

class arborx_search: public ::testing::Test
{
  void SetUp() override
  {
    search::initialize_otm_search();
  }

  void TearDown() override
  {
    search::finalize_otm_search();
  }
};

TEST_F(arborx_search, canInitializeArborXNodesFromOTMNodes)
{
  state s;

  tetrahedron_single_point(s);

  auto search_nodes = search::arborx::create_arborx_nodes(s);

  EXPECT_EQ(search_nodes.extent(0), 4);

  auto nodes_to_x = s.x.cbegin();
  auto node_check_func = HPC_DEVICE [=](node_index node)
  {
    auto&& lgr_node_coord = nodes_to_x[node].load();
    auto&& search_node_coord = search_nodes(hpc::weaken(node));
    EXPECT_DOUBLE_EQ(lgr_node_coord(0), search_node_coord[0]);
    EXPECT_DOUBLE_EQ(lgr_node_coord(1), search_node_coord[1]);
    EXPECT_DOUBLE_EQ(lgr_node_coord(2), search_node_coord[2]);
  };

  hpc::for_each(hpc::device_policy(), s.nodes, node_check_func);
}

TEST_F(arborx_search, canInitializeArborXPointsFromOTMPoints)
{
  state s;

  tetrahedron_single_point(s);

  auto search_points = search::arborx::create_arborx_points(s);

  EXPECT_EQ(search_points.extent(0), 1);

  auto points_to_x = s.xm.cbegin();
  auto pt_check_func = HPC_DEVICE [=](point_index point)
  {
    auto&& lgr_pt_coord = points_to_x[point].load();
    auto&& search_pt_coord = search_points(hpc::weaken(point));
    EXPECT_DOUBLE_EQ(lgr_pt_coord(0), search_pt_coord[0]);
    EXPECT_DOUBLE_EQ(lgr_pt_coord(1), search_pt_coord[1]);
    EXPECT_DOUBLE_EQ(lgr_pt_coord(2), search_pt_coord[2]);
  };

  hpc::for_each(hpc::device_policy(), s.points, pt_check_func);
}

TEST_F(arborx_search, canDoNearestNodePointSearch)
{
  state s;
  tetrahedron_single_point(s);

  auto search_nodes = search::arborx::create_arborx_nodes(s);
  auto search_points = search::arborx::create_arborx_points(s);
  const int num_nodes_per_point_to_find = 4;
  auto queries = search::arborx::make_nearest_node_queries(search_points,
      num_nodes_per_point_to_find);

  search::arborx::device_int_view offsets;
  search::arborx::device_int_view indices;
  Kokkos::tie(offsets, indices) = search::arborx::do_search(search_nodes,
      queries);

  int num_points = search_points.extent(0);
  auto nodes_to_x = s.x.cbegin();

  EXPECT_EQ(num_points, 1);

  auto pt_check_func = HPC_DEVICE [=](point_index point)
  {
    auto point_begin = offsets(hpc::weaken(point));
    auto point_end = offsets(hpc::weaken(point)+1);
    EXPECT_EQ(point_end - point_begin, 4);
    for (auto j=point_begin; j<point_end; ++j)
    {
      auto search_node_coord = search_nodes(indices(j));
      auto&& lgr_node_coord = nodes_to_x[indices(j)].load();
      EXPECT_DOUBLE_EQ(lgr_node_coord(0), search_node_coord[0]);
      EXPECT_DOUBLE_EQ(lgr_node_coord(1), search_node_coord[1]);
      EXPECT_DOUBLE_EQ(lgr_node_coord(2), search_node_coord[2]);

    }
  };

  hpc::for_each(hpc::device_policy(), s.points, pt_check_func);
}

TEST_F(arborx_search, canDoNearestNodePointSearchThroughLGRInterface)
{
  state s;
  tetrahedron_single_point(s);

  hpc::device_vector<node_index, point_node_index> points_to_supported_nodes_before_search(s.points_to_supported_nodes.size());
  hpc::copy(s.points_to_supported_nodes, points_to_supported_nodes_before_search);

  search::do_otm_point_node_search(s);

  auto points_to_nodes_of_point = s.nodes_in_support.cbegin();
  auto old_points_to_supported_nodes = points_to_supported_nodes_before_search.cbegin();
  auto new_points_to_supported_nodes = s.points_to_supported_nodes.cbegin();
  auto pt_node_check_func = HPC_DEVICE [=](lgr::point_index point) {
    auto point_node_range = points_to_nodes_of_point[point];
    EXPECT_EQ(point_node_range.size(), 4);
    for (auto point_node : point_node_range)
    {
      EXPECT_EQ(old_points_to_supported_nodes[point_node], new_points_to_supported_nodes[point_node]);
    }
  };

  hpc::for_each(hpc::device_policy(), s.points, pt_node_check_func);
}

TEST_F(arborx_search, canDoNearestNodePointSearchOnExodusMesh)
{
  material_index mat(1);
  material_index bnd(1);
  input in(mat, bnd);
  state st;
  in.element = MESHLESS;

  int err_code = read_exodus_file("tets.g", in, st);

  ASSERT_EQ(err_code, 0);

  convert_tet_mesh_to_meshless(st);

  hpc::device_vector<node_index, point_node_index> points_to_supported_nodes_before_search(
      st.points_to_supported_nodes.size());
  hpc::copy(st.points_to_supported_nodes, points_to_supported_nodes_before_search);

  search::do_otm_point_node_search(st);

  auto points_to_nodes_of_point = st.nodes_in_support.cbegin();
  auto old_points_to_supported_nodes = points_to_supported_nodes_before_search.cbegin();
  auto new_points_to_supported_nodes = st.points_to_supported_nodes.cbegin();
  auto pt_node_check_func =
      HPC_DEVICE [=](
          point_index point)
          {
            auto point_node_range = points_to_nodes_of_point[point];
            EXPECT_EQ(point_node_range.size(), 4);
            for (auto point_node : point_node_range)
            {
              EXPECT_EQ(old_points_to_supported_nodes[point_node], new_points_to_supported_nodes[point_node]);
            }
          };

  hpc::for_each(hpc::device_policy(), st.points, pt_node_check_func);
}
