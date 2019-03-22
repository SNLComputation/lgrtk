#include <lgr_meshing.hpp>
#include <lgr_macros.hpp>
#include <lgr_state.hpp>
#include <lgr_fill.hpp>
#include <lgr_int_range_product.hpp>
#include <lgr_input.hpp>

namespace lgr {

static void LGR_NOINLINE invert_connectivity(
    int_range const nodes,
    int_range const elements,
    int_range const nodes_in_element,
    host_vector<int> const& elements_to_nodes_vector,
    int_range_sum<host_allocator<int>>* nodes_to_node_elements_vector,
    host_vector<int>* node_elements_to_elements_vector,
    host_vector<int>* node_elements_to_nodes_in_element_vector) {
  host_vector<int> counts_vector(nodes.size());
  auto const counts_iterator = counts_vector.begin();
  lgr::fill(counts_vector, int(0));
  auto const elements_to_element_nodes = elements * nodes_in_element;
  auto const element_nodes_to_nodes = elements_to_nodes_vector.cbegin();
  auto count_functor = [=](int const element) {
    auto const element_nodes = elements_to_element_nodes[element];
    for (auto const element_node : element_nodes) {
      int const node = element_nodes_to_nodes[element_node];
      { // needs to be atomic!
      int count = counts_iterator[node];
      ++count;
      counts_iterator[node] = count;
      }
    }
  };
  lgr::for_each(elements, count_functor);
  nodes_to_node_elements_vector->assign_sizes(counts_vector.get_array_vector().get_vector());
  lgr::fill(counts_vector, int(0));
  auto const nodes_to_node_elements_iterator = nodes_to_node_elements_vector->cbegin();
  auto const node_elements_to_elements_iterator = node_elements_to_elements_vector->begin();
  auto const node_elements_to_nodes_in_element_iterator = node_elements_to_nodes_in_element_vector->begin();
  auto fill_functor = [=](int const element) {
    auto const element_nodes = elements_to_element_nodes[element];
    for (auto const node_in_element : nodes_in_element) {
      auto const element_node = element_nodes[node_in_element];
      auto const node = element_nodes_to_nodes[element_node];
      int offset;
      { // needs to be atomic!
      offset = counts_iterator[node];
      counts_iterator[node] = offset + 1;
      }
      auto const node_elements_range = nodes_to_node_elements_iterator[node];
      auto const node_element = node_elements_range[offset];
      node_elements_to_elements_iterator[node_element] = element;
      node_elements_to_nodes_in_element_iterator[node_element] = node_in_element;
    }
  };
  lgr::for_each(elements, fill_functor);
}

static void LGR_NOINLINE initialize_bars_to_nodes(
    int_range const elements,
    host_vector<int>* elements_to_nodes) {
  auto const begin = elements_to_nodes->begin();
  auto functor = [=] (int const element) {
    begin[element * 2 + 0] = element;
    begin[element * 2 + 1] = element + 1;
  };
  lgr::for_each(elements, functor);
}

static void LGR_NOINLINE initialize_x_1D(input const& in, int_range const nodes, decltype(state::x)* x_vector) {
  auto const nodes_to_x = x_vector->begin();
  auto const num_nodes = nodes.size();
  auto const l = in.x_domain_size;
  auto functor = [=](int const node) {
    nodes_to_x[node] = vector3<double>(l * (double(node) / double(num_nodes - 1)), 0.0, 0.0);
  };
  lgr::for_each(nodes, functor);
}

static void build_bar_mesh(input const& in, state& s) {
  s.elements.resize(in.elements_along_x);
  s.nodes_in_element.resize(2);
  s.nodes.resize(s.elements.size() + 1);
  s.elements_to_nodes.resize(s.elements.size() * s.nodes_in_element.size());
  initialize_bars_to_nodes(s.nodes, &s.elements_to_nodes);
  s.x.resize(s.nodes.size());
  initialize_x_1D(in, s.nodes, &s.x);
}

static void LGR_NOINLINE build_triangle_mesh(input const& in, state& s)
{
  assert(in.elements_along_x >= 1);
  int const nx = in.elements_along_x;
  assert(in.elements_along_y >= 1);
  int const ny = in.elements_along_y;
  s.nodes_in_element.resize(3);
  int const nvx = nx + 1;
  int const nvy = ny + 1;
  int const nv = nvx * nvy;
  s.nodes.resize(nv);
  int const nq = nx * ny;
  int const nt = nq * 2;
  s.elements.resize(nt);
  s.elements_to_nodes.resize(s.elements.size() * s.nodes_in_element.size());
  auto const elements_to_nodes = s.elements_to_nodes.begin();
  auto connectivity_functor = [=] (int const quad) {
    int const i = quad % nx;
    int const j = quad / nx;
    elements_to_nodes[(quad * 2 + 0) * 3 + 0] = (j + 0) * nvx + (i + 0);
    elements_to_nodes[(quad * 2 + 0) * 3 + 1] = (j + 0) * nvx + (i + 1);
    elements_to_nodes[(quad * 2 + 0) * 3 + 2] = (j + 1) * nvx + (i + 1);
    elements_to_nodes[(quad * 2 + 1) * 3 + 0] = (j + 1) * nvx + (i + 1);
    elements_to_nodes[(quad * 2 + 1) * 3 + 1] = (j + 1) * nvx + (i + 0);
    elements_to_nodes[(quad * 2 + 1) * 3 + 2] = (j + 0) * nvx + (i + 0);
  };
  int_range quads(nq);
  lgr::for_each(quads, connectivity_functor);
  s.x.resize(s.nodes.size());
  auto const nodes_to_x = s.x.begin();
  double const x = in.x_domain_size;
  double const y = in.y_domain_size;
  double const dx = x / nx;
  double const dy = y / ny;
  auto coordinates_functor = [=] (int const node) {
    int const i = node % nvx;
    int const j = node / nvx;
    nodes_to_x[node] = vector3<double>(i * dx, j * dy, 0.0);
  };
  lgr::for_each(s.nodes, coordinates_functor);
}

static void LGR_NOINLINE build_tetrahedron_mesh(input const& in, state& s)
{
  assert(in.elements_along_x >= 1);
  int const nx = in.elements_along_x;
  assert(in.elements_along_y >= 1);
  int const ny = in.elements_along_y;
  assert(in.elements_along_z >= 1);
  int const nz = in.elements_along_z;
  s.nodes_in_element.resize(4);
  int const nvx = nx + 1;
  int const nvy = ny + 1;
  int const nvz = nz + 1;
  int const nvxy = nvx * nvy;
  int const nv = nvxy * nvz;
  s.nodes.resize(nv);
  int const nxy = nx * ny;
  int const nh = nxy * nz;
  int const nt = nh * 6;
  s.elements.resize(nt);
  s.elements_to_nodes.resize(s.elements.size() * s.nodes_in_element.size());
  auto const elements_to_nodes = s.elements_to_nodes.begin();
  auto connectivity_functor = [=] (int const hex) {
    int const ij = hex % nxy;
    int const k = hex / nxy;
    int const i = ij % nx;
    int const j = ij / nx;
    int hex_nodes[8];
    hex_nodes[0] = ((k + 0) * nvy + (j + 0)) * nvx + (i + 0);
    hex_nodes[1] = ((k + 0) * nvy + (j + 0)) * nvx + (i + 1);
    hex_nodes[2] = ((k + 0) * nvy + (j + 1)) * nvx + (i + 0);
    hex_nodes[3] = ((k + 0) * nvy + (j + 1)) * nvx + (i + 1);
    hex_nodes[4] = ((k + 1) * nvy + (j + 0)) * nvx + (i + 0);
    hex_nodes[5] = ((k + 1) * nvy + (j + 0)) * nvx + (i + 1);
    hex_nodes[6] = ((k + 1) * nvy + (j + 1)) * nvx + (i + 0);
    hex_nodes[7] = ((k + 1) * nvy + (j + 1)) * nvx + (i + 1);
    elements_to_nodes[(hex * 6 + 0) * 4 + 0] = hex_nodes[0];
    elements_to_nodes[(hex * 6 + 0) * 4 + 1] = hex_nodes[3];
    elements_to_nodes[(hex * 6 + 0) * 4 + 2] = hex_nodes[2];
    elements_to_nodes[(hex * 6 + 0) * 4 + 3] = hex_nodes[7];
    elements_to_nodes[(hex * 6 + 1) * 4 + 0] = hex_nodes[0];
    elements_to_nodes[(hex * 6 + 1) * 4 + 1] = hex_nodes[2];
    elements_to_nodes[(hex * 6 + 1) * 4 + 2] = hex_nodes[6];
    elements_to_nodes[(hex * 6 + 1) * 4 + 3] = hex_nodes[7];
    elements_to_nodes[(hex * 6 + 2) * 4 + 0] = hex_nodes[0];
    elements_to_nodes[(hex * 6 + 2) * 4 + 1] = hex_nodes[6];
    elements_to_nodes[(hex * 6 + 2) * 4 + 2] = hex_nodes[4];
    elements_to_nodes[(hex * 6 + 2) * 4 + 3] = hex_nodes[7];
    elements_to_nodes[(hex * 6 + 3) * 4 + 0] = hex_nodes[0];
    elements_to_nodes[(hex * 6 + 3) * 4 + 1] = hex_nodes[5];
    elements_to_nodes[(hex * 6 + 3) * 4 + 2] = hex_nodes[7];
    elements_to_nodes[(hex * 6 + 3) * 4 + 3] = hex_nodes[4];
    elements_to_nodes[(hex * 6 + 4) * 4 + 0] = hex_nodes[1];
    elements_to_nodes[(hex * 6 + 4) * 4 + 1] = hex_nodes[5];
    elements_to_nodes[(hex * 6 + 4) * 4 + 2] = hex_nodes[7];
    elements_to_nodes[(hex * 6 + 4) * 4 + 3] = hex_nodes[0];
    elements_to_nodes[(hex * 6 + 5) * 4 + 0] = hex_nodes[1];
    elements_to_nodes[(hex * 6 + 5) * 4 + 1] = hex_nodes[7];
    elements_to_nodes[(hex * 6 + 5) * 4 + 2] = hex_nodes[3];
    elements_to_nodes[(hex * 6 + 5) * 4 + 3] = hex_nodes[0];
  };
  int_range hexes(nh);
  lgr::for_each(hexes, connectivity_functor);
  s.x.resize(s.nodes.size());
  auto const nodes_to_x = s.x.begin();
  double const x = in.x_domain_size;
  double const y = in.y_domain_size;
  double const z = in.z_domain_size;
  double const dx = x / nx;
  double const dy = y / ny;
  double const dz = z / nz;
  auto coordinates_functor = [=] (int const node) {
    int const ij = node % nvxy;
    int const k = node / nvxy;
    int const i = ij % nvx;
    int const j = ij / nvx;
    nodes_to_x[node] = vector3<double>(i * dx, j * dy, k * dz);
  };
  lgr::for_each(s.nodes, coordinates_functor);
}

void build_mesh(input const& in, state& s) {
  switch (in.element) {
    case BAR: build_bar_mesh(in, s); break;
    case TRIANGLE: build_triangle_mesh(in, s); break;
    case TETRAHEDRON: build_tetrahedron_mesh(in, s); break;
  }
  s.nodes_to_node_elements.resize(s.nodes.size());
  s.node_elements_to_elements.resize(s.elements.size() * s.nodes_in_element.size());
  s.node_elements_to_nodes_in_element.resize(s.elements.size() * s.nodes_in_element.size());
  invert_connectivity(s.nodes, s.elements, s.nodes_in_element, 
      s.elements_to_nodes, &s.nodes_to_node_elements,
      &s.node_elements_to_elements, &s.node_elements_to_nodes_in_element);
}

}