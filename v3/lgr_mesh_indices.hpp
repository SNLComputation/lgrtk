#pragma once

#include <cstdint>

#include <lgr_index.hpp>

namespace lgr {

class node_index : public index<int, node_index> {
  public:
    using base_type = index<int, node_index>;
    constexpr explicit inline node_index(int const i) noexcept : base_type(i) {}
    inline node_index() noexcept = default;
};

class node_in_element_index : public index<int, node_in_element_index> {
  public:
    using base_type = index<int, node_in_element_index>;
    constexpr explicit inline node_in_element_index(int const i) noexcept : base_type(i) {}
};

class element_index : public index<int, element_index> {
  public:
    using base_type = index<int, element_index>;
    constexpr explicit inline element_index(int const i) noexcept : base_type(i) {}
    inline element_index() noexcept = default;
};

class element_node_index : public index<int, element_node_index> {
  public:
    using base_type = index<int, element_node_index>;
    constexpr explicit inline element_node_index(int const i) noexcept : base_type(i) {}
};

constexpr inline element_node_index operator*(element_index const& e, node_in_element_index const& n) noexcept {
  return element_node_index(int(e) * int(n));
}

class node_element_index : public index<int, node_element_index> {
  public:
    using base_type = index<int, node_element_index>;
    constexpr explicit inline node_element_index(int const i) noexcept : base_type(i) {}
};

class point_index : public index<int, point_index> {
  public:
    using base_type = index<int, point_index>;
    constexpr explicit inline point_index(int const i) noexcept : base_type(i) {}
};

class point_in_element_index : public index<int, point_in_element_index> {
  public:
    using base_type = index<int, point_in_element_index>;
    constexpr explicit inline point_in_element_index(int const i) noexcept : base_type(i) {}
};

constexpr inline point_index operator*(element_index const& e, point_in_element_index const& qp) noexcept {
  return point_index(int(e) * int(qp));
}

class point_node_index : public index<int, point_node_index> {
  public:
    using base_type = index<int, point_node_index>;
    constexpr explicit inline point_node_index(int const i) noexcept : base_type(i) {}
};

constexpr inline point_node_index operator*(point_index const& p, node_in_element_index const& n) noexcept {
  return point_node_index(int(p) * int(n));
}

class material_index : public index<int, material_index> {
  public:
    using base_type = index<int, material_index>;
    constexpr explicit inline material_index(int const i) noexcept : base_type(i) {}
    inline material_index() noexcept = default;
};

}
