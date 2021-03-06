#pragma once

#include <cstdint>
#include <hpc_index.hpp>

namespace lgr {

struct node_tag
{
};
using node_index = hpc::index<node_tag, int>;
struct node_in_element_tag
{
};
using node_in_element_index = hpc::index<node_in_element_tag, int>;
struct element_tag
{
};
using element_index = hpc::index<element_tag, int>;
struct node_element_tag
{
};
using node_element_index = hpc::index<node_element_tag, int>;
struct point_in_element_tag
{
};
using point_in_element_index = hpc::index<point_in_element_tag, int>;
using point_index            = decltype(element_index() * point_in_element_index());
using element_node_index     = decltype(element_index() * node_in_element_index());
struct point_node_tag
{
};
using point_node_index = hpc::index<point_node_tag, int>;
struct node_point_tag
{
};
using node_point_index = hpc::index<node_point_tag, int>;
struct material_tag
{
};
using material_index = hpc::index<material_tag, int>;

}  // namespace lgr
