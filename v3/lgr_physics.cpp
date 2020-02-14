#include <iostream>
#include <cassert>
#include <iomanip>
#include <iostream>  // DEBUGGING

#include <lgr_state.hpp>
#include <lgr_physics.hpp>
#include <hpc_symmetric3x3.hpp>
#include <hpc_macros.hpp>
#include <lgr_print.hpp>
#include <lgr_vtk.hpp>
#include <lgr_element_specific.hpp>
#include <lgr_meshing.hpp>
#include <lgr_input.hpp>
#include <lgr_stabilized.hpp>
#include <lgr_adapt.hpp>
#if defined(HYPER_EP)
#include <lgr_hyper_ep/model.hpp>
#endif
#include <j2/hardening.hpp>

namespace lgr {

HPC_NOINLINE inline void advance_time(
    input const& in,
    hpc::time<double> const max_stable_dt,
    hpc::time<double> const next_file_output_time,
    hpc::time<double>* time,
    hpc::time<double>* dt) {
  auto const old_time = *time;
  auto new_time = next_file_output_time;
  new_time = std::min(new_time, old_time + (max_stable_dt * in.CFL));
  *time = new_time;
  *dt = new_time - old_time;
}

HPC_NOINLINE inline void update_u(state& s, hpc::time<double> const dt) {
  auto const nodes_to_u = s.u.begin();
  auto const nodes_to_v = s.v.cbegin();
  auto functor = [=] HPC_DEVICE (node_index const node) {
    auto const old_u = nodes_to_u[node].load();
    auto const v = nodes_to_v[node].load();
    auto const u = (dt * v) - old_u;
    nodes_to_u[node] = u;
  };
  hpc::for_each(hpc::device_policy(), s.nodes, functor);
}

HPC_NOINLINE inline void explicit_newmark_predict(state& s, hpc::time<double> const dt) {
  auto const nodes_to_u = s.u.begin();
  auto const nodes_to_v = s.v.begin();
  auto const nodes_to_a = s.a.cbegin();
  auto functor = [=] HPC_DEVICE (node_index const node) {
    auto const u = nodes_to_u[node].load();
    auto const v = nodes_to_v[node].load();
    auto const a = nodes_to_a[node].load();
    auto const vp = 0.5 * dt * a;
    auto const u_pred = u + (dt * v) + (dt * vp);
    auto const v_pred = v + vp;
    nodes_to_u[node] = u_pred;
    nodes_to_v[node] = v_pred;
  };
  hpc::for_each(hpc::device_policy(), s.nodes, functor);
}

HPC_NOINLINE inline void explicit_newmark_correct(state& s, hpc::time<double> const dt) {
  auto const nodes_to_u = s.u.begin();
  auto const nodes_to_v = s.v.begin();
  auto const nodes_to_a = s.a.cbegin();
  auto functor = [=] HPC_DEVICE (node_index const node) {
    auto const u = nodes_to_u[node].load();
    auto const v = nodes_to_v[node].load();
    auto const a = nodes_to_a[node].load();
    auto const vp = dt * a;
    auto const u_corr = u + (dt * vp);
    auto const v_corr = v + vp;
    nodes_to_u[node] = u_corr;
    nodes_to_v[node] = v_corr;
  };
  hpc::for_each(hpc::device_policy(), s.nodes, functor);
}

HPC_NOINLINE inline void update_v(state& s, hpc::time<double> const dt,
    hpc::device_array_vector<hpc::velocity<double>, node_index> const& old_v_vector) {
  auto const nodes_to_v = s.v.begin();
  auto const nodes_to_old_v = old_v_vector.cbegin();
  auto const nodes_to_a = s.a.cbegin();
  auto functor = [=] HPC_DEVICE (node_index const node) {
    auto const old_v = nodes_to_old_v[node].load();
    auto const a = nodes_to_a[node].load();
    auto const v = old_v + dt * a;
    nodes_to_v[node] = v;
  };
  hpc::for_each(hpc::device_policy(), s.nodes, functor);
}

HPC_NOINLINE inline void update_a(state& s) {
  auto const nodes_to_f = s.f.cbegin();
  auto const nodes_to_m = s.mass.cbegin();
  auto const nodes_to_a = s.a.begin();
  auto functor = [=] HPC_DEVICE (node_index const node) {
    auto const f = nodes_to_f[node].load();
    auto const m = nodes_to_m[node];
    auto const a = f / m;
    nodes_to_a[node] = a;
  };
  hpc::for_each(hpc::device_policy(), s.nodes, functor);
}

HPC_NOINLINE inline void update_x(state& s) {
  auto const nodes_to_u = s.u.cbegin();
  auto const nodes_to_x = s.x.begin();
  auto functor = [=] HPC_DEVICE (node_index const node) {
    auto const old_x = nodes_to_x[node].load();
    auto const u = nodes_to_u[node].load();
    auto const new_x = old_x + u;
    nodes_to_x[node] = new_x;
  };
  hpc::for_each(hpc::device_policy(), s.nodes, functor);
}

HPC_NOINLINE inline void update_p(state& s, material_index const material) {
  auto const points_to_sigma = s.sigma.cbegin();
  auto const points_to_p = s.p.begin();
  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    for (auto const point : elements_to_points[element]) {
      auto const sigma = points_to_sigma[point].load();
      auto const p = -(1.0 / 3.0) * trace(sigma);
      points_to_p[point] = p;
    }
  };
  hpc::for_each(hpc::device_policy(), s.element_sets[material], functor);
}

HPC_NOINLINE inline void update_reference(state& s) {
  auto const elements_to_element_nodes = s.elements * s.nodes_in_element;
  auto const elements_to_element_points = s.elements * s.points_in_element;
  auto const points_to_point_nodes = s.points * s.nodes_in_element;
  auto const element_nodes_to_nodes = s.elements_to_nodes.cbegin();
  auto const nodes_to_u = s.u.cbegin();
  auto const points_to_F_total = s.F_total.begin();
  auto const point_nodes_to_grad_N = s.grad_N.begin();
  auto const points_to_V = s.V.begin();
  auto const points_to_rho = s.rho.begin();
  auto const nodes_in_element = s.nodes_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    auto const element_nodes = elements_to_element_nodes[element];
    auto const element_points = elements_to_element_points[element];
    for (auto const point : element_points) {
      auto const point_nodes = points_to_point_nodes[point];
      auto F_incr = hpc::deformation_gradient<double>::identity();
      for (auto const node_in_element : nodes_in_element) {
        auto const element_node = element_nodes[node_in_element];
        auto const point_node = point_nodes[node_in_element];
        auto const node = element_nodes_to_nodes[element_node];
        auto const u = nodes_to_u[node].load();
        auto const old_grad_N = point_nodes_to_grad_N[point_node].load();
        F_incr = F_incr + outer_product(u, old_grad_N);
      }
      auto const F_inverse_transpose = transpose(inverse(F_incr));
      for (auto const point_node : point_nodes) {
        auto const old_grad_N = point_nodes_to_grad_N[point_node].load();
        auto const new_grad_N = F_inverse_transpose * old_grad_N;
        point_nodes_to_grad_N[point_node] = new_grad_N;
      }
      auto const old_F_total = points_to_F_total[point].load();
      auto const new_F_total = F_incr * old_F_total;
      points_to_F_total[point] = new_F_total;
      auto const J = determinant(F_incr);
      assert(J > 0.0);
      auto const old_V = points_to_V[point];
      auto const new_V = J * old_V;
      assert(new_V > 0.0);
      points_to_V[point] = new_V;
      auto const old_rho = points_to_rho[point];
      auto const new_rho = old_rho / J;
      points_to_rho[point] = new_rho;
    }
  };
  hpc::for_each(hpc::device_policy(), s.elements, functor);
}

HPC_NOINLINE inline void update_c(state& s)
{
  auto const points_to_rho = s.rho.cbegin();
  auto const points_to_K = s.K.cbegin();
  auto const points_to_G = s.G.cbegin();
  auto const points_to_c = s.c.begin();
  auto functor = [=] HPC_DEVICE (point_index const point) {
    auto const rho = points_to_rho[point];
    auto const K = points_to_K[point];
    auto const G = points_to_G[point];
    auto const M = K + (4.0 / 3.0) * G;
    auto const c = sqrt(M / rho);
    points_to_c[point] = c;
  };
  hpc::for_each(hpc::device_policy(), s.points, functor);
}

HPC_NOINLINE inline void update_element_dt(state& s) {
  auto const points_to_c = s.c.cbegin();
  auto const elements_to_h_min = s.h_min.cbegin();
  auto const points_to_nu_art = s.nu_art.cbegin();
  auto const points_to_dt = s.element_dt.begin();
  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    auto const h_min = elements_to_h_min[element];
    for (auto const point : elements_to_points[element]) {
      auto const c = points_to_c[point];
      auto const nu_art = points_to_nu_art[point];
      auto const h_sq = h_min * h_min;
      auto const c_sq = c * c;
      auto const nu_art_sq = nu_art * nu_art;
      auto const dt = h_sq / (nu_art + sqrt(nu_art_sq + (c_sq * h_sq)));
      assert(dt > 0.0);
      points_to_dt[point] = dt;
    }
  };
  hpc::for_each(hpc::device_policy(), s.elements, functor);
}

HPC_NOINLINE inline void find_max_stable_dt(state& s)
{
  hpc::time<double> const init(std::numeric_limits<double>::max());
  s.max_stable_dt = hpc::transform_reduce(
      hpc::device_policy(),
      s.element_dt,
      init,
      hpc::minimum<hpc::time<double>>(),
      hpc::identity<hpc::time<double>>());
  assert(s.max_stable_dt < 1.0);
}

HPC_NOINLINE inline void neo_Hookean(input const& in, state& s, material_index const material) {
  auto const points_to_F_total = s.F_total.cbegin();
  auto const points_to_sigma = s.sigma.begin();
  auto const points_to_K = s.K.begin();
  auto const points_to_G = s.G.begin();
  auto const K0 = in.K0[material];
  auto const G0 = in.G0[material];
  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    for (auto const point : elements_to_points[element]) {
      auto const F = points_to_F_total[point].load();
      auto const J = determinant(F);
      auto const Jinv = 1.0 / J;
      auto const half_K0 = 0.5 * K0;
      auto const Jm13 = 1.0 / cbrt(J);
      auto const Jm23 = Jm13 * Jm13;
      auto const Jm53 = (Jm23 * Jm23) * Jm13;
      auto const B = self_times_transpose(F);
      auto const devB = deviatoric_part(B);
      auto const sigma = half_K0 * (J - Jinv) + (G0 * Jm53) * devB;
      points_to_sigma[point] = sigma;
      auto const K = half_K0 * (J + Jinv);
      points_to_K[point] = K;
      points_to_G[point] = G0;
    }
  };
  hpc::for_each(hpc::device_policy(), s.element_sets[material], functor);
}

#if defined(HYPER_EP)
HPC_NOINLINE inline void hyper_ep_update(input const& in, state& s, material_index const material) {
  // Constant state
  auto const points_to_dt = s.dt;
  auto const points_to_F_total = s.F_total.cbegin();
  auto const points_to_temp = s.temp.cbegin();

  // Variables to be updated
  auto points_to_Fp_total = s.Fp_total.begin();
  auto points_to_sigma = s.sigma.begin();
  auto points_to_ep = s.ep.begin();
  auto points_to_ep_dot = s.ep_dot.begin();
  auto points_to_dp = s.dp.begin();
  auto points_to_localized = s.localized.begin();

  // Elastic parameters to be updated
  auto const points_to_K = s.K.begin();
  auto const points_to_G = s.G.begin();

  // Material properties
  hyper_ep::Properties props;
  props.elastic = in.elastic[material];
  props.E = in.E[material];
  props.nu = in.nu[material];
  props.hardening = in.hardening[material];
  props.A = in.A[material];
  props.B = in.B[material];
  props.n = in.n[material];
  props.C1 = in.C1[material];
  props.C2 = in.C2[material];
  props.C3 = in.C3[material];
  props.C4 = in.C4[material];
  props.damage = in.damage[material];
  props.allow_no_tension = in.allow_no_tension[material];
  props.allow_no_shear = in.allow_no_shear[material];
  props.set_stress_to_zero = in.set_stress_to_zero[material];
  props.D1 = in.D1[material];
  props.D2 = in.D2[material];
  props.D3 = in.D3[material];
  props.D4 = in.D4[material];
  props.D5 = in.D5[material];
  props.D6 = in.D6[material];
  props.D7 = in.D7[material];
  props.DC = in.DC[material];
  props.eps_f_min = in.eps_f_min[material];

  // Derived properties
  auto const K0 = props.E / 3. / (1. - 2. * props.nu);
  auto const G0 = props.E / 2. / (1. + props.nu);

  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    for (auto const point : elements_to_points[element]) {
      auto const dt = points_to_dt;
      auto const F = points_to_F_total[point].load();
      auto const temp = points_to_temp[point];
      auto Fp = points_to_Fp_total[point].load();
      auto T = points_to_sigma[point].load();

      auto ep = points_to_ep[point];
      auto ep_dot = points_to_ep_dot[point];
      auto dp = points_to_dp[point];
      auto localized = points_to_localized[point];

      auto err_c = hyper_ep::update(
         props, F, dt, temp, T, Fp, ep, ep_dot, dp, localized);
      assert(err_c == hyper_ep::ErrorCode::SUCCESS);

      points_to_Fp_total[point] = Fp;
      points_to_ep[point] = ep;
      points_to_ep_dot[point] = ep_dot;
      points_to_dp[point] = dp;
      points_to_localized[point] = localized;
      points_to_sigma[point] = T;

      auto const J = determinant(F);
      auto const Jinv = 1.0 / J;
      auto const half_K0 = 0.5 * K0;
      auto const K = half_K0 * (J + Jinv);
      points_to_K[point] = K;
      points_to_G[point] = G0;
    }
  };
  hpc::for_each(hpc::device_policy(), s.element_sets[material], functor);
}
#endif // HYPER_EP

HPC_NOINLINE inline void ideal_gas(input const& in, state& s, material_index const material) {
  auto const points_to_rho = s.rho.cbegin();
  auto const points_to_e = s.e.cbegin();
  auto const points_to_sigma = s.sigma.begin();
  auto const points_to_K = s.K.begin();
  auto const gamma = in.gamma[material];
  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    for (auto const point : elements_to_points[element]) {
      auto const rho = points_to_rho[point];
      assert(rho > 0.0);
      auto const e = points_to_e[point];
      assert(e > 0.0);
      auto const p = (gamma - 1.0) * (rho * e);
      assert(p > 0.0);
      auto const old_sigma = points_to_sigma[point].load();
      auto const new_sigma = deviatoric_part(old_sigma) - p;
      points_to_sigma[point] = new_sigma;
      auto const K = gamma * p;
      assert(K > 0.0);
      points_to_K[point] = K;
    }
  };
  hpc::for_each(hpc::device_policy(), s.element_sets[material], functor);
}

HPC_NOINLINE inline void update_element_force(state& s)
{
  auto const points_to_sigma = s.sigma.cbegin();
  auto const points_to_V = s.V.cbegin();
  auto const point_nodes_to_grad_N = s.grad_N.cbegin();
  auto const point_nodes_to_f = s.element_f.begin();
  auto const points_to_point_nodes = s.points * s.nodes_in_element;
  auto functor = [=] HPC_DEVICE (point_index const point) {
    auto const sigma = points_to_sigma[point].load();
    auto const V = points_to_V[point];
    auto const point_nodes = points_to_point_nodes[point];
    for (auto const point_node : point_nodes) {
      auto const grad_N = point_nodes_to_grad_N[point_node].load();
      auto const f = -(sigma * grad_N) * V;
      point_nodes_to_f[point_node] = f;
    }
  };
  hpc::for_each(hpc::device_policy(), s.points, functor);
}

HPC_NOINLINE inline void update_nodal_force(state& s) {
  auto const nodes_to_node_elements = s.nodes_to_node_elements.cbegin();
  auto const node_elements_to_elements = s.node_elements_to_elements.cbegin();
  auto const node_elements_to_nodes_in_element = s.node_elements_to_nodes_in_element.cbegin();
  auto const point_nodes_to_f = s.element_f.cbegin();
  auto const nodes_to_f = s.f.begin();
  auto const points_to_point_nodes = s.points * s.nodes_in_element;
  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (node_index const node) {
    auto node_f = hpc::force<double>::zero();
    auto const node_elements = nodes_to_node_elements[node];
    for (auto const node_element : node_elements) {
      auto const element = node_elements_to_elements[node_element];
      auto const node_in_element = node_elements_to_nodes_in_element[node_element];
      for (auto const point : elements_to_points[element]) {
        auto const point_nodes = points_to_point_nodes[point];
        auto const point_node = point_nodes[node_in_element];
        auto const point_f = point_nodes_to_f[point_node].load();
        node_f = node_f + point_f;
      }
    }
    nodes_to_f[node] = node_f;
  };
  hpc::for_each(hpc::device_policy(), s.nodes, functor);
}

HPC_NOINLINE inline void zero_acceleration(
    hpc::device_vector<node_index, int> const& domain,
    hpc::vector3<double> const axis,
    hpc::device_array_vector<hpc::acceleration<double>, node_index>* a_vector) {
  auto const nodes_to_a = a_vector->begin();
  auto functor = [=] HPC_DEVICE (node_index const node) {
    auto const old_a = nodes_to_a[node].load();
    auto const new_a = old_a - axis * (old_a * axis);
    nodes_to_a[node] = new_a;
  };
  hpc::for_each(hpc::device_policy(), domain, functor);
}

HPC_NOINLINE inline void update_symm_grad_v(state& s)
{
  auto const elements_to_element_nodes = s.elements * s.nodes_in_element;
  auto const elements_to_points = s.elements * s.points_in_element;
  auto const points_to_point_nodes = s.points * s.nodes_in_element;
  auto const element_nodes_to_nodes = s.elements_to_nodes.cbegin();
  auto const point_nodes_to_grad_N = s.grad_N.cbegin();
  auto const nodes_to_v = s.v.cbegin();
  auto const points_to_symm_grad_v = s.symm_grad_v.begin();
  auto const nodes_in_element = s.nodes_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    for (auto const point : elements_to_points[element]) {
      auto grad_v = hpc::velocity_gradient<double>::zero();
      auto const element_nodes = elements_to_element_nodes[element];
      auto const point_nodes = points_to_point_nodes[point];
      for (auto const node_in_element : nodes_in_element) {
        auto const element_node = element_nodes[node_in_element];
        auto const point_node = point_nodes[node_in_element];
        node_index const node = element_nodes_to_nodes[element_node];
        auto const v = nodes_to_v[node].load();
        auto const grad_N = point_nodes_to_grad_N[point_node].load();
        grad_v = grad_v + outer_product(v, grad_N);
      }
      hpc::symmetric_velocity_gradient<double> const symm_grad_v(grad_v);
      points_to_symm_grad_v[point] = symm_grad_v;
    }
  };
  hpc::for_each(hpc::device_policy(), s.elements, functor);
}

HPC_NOINLINE inline void stress_power(state& s)
{
  auto const points_to_sigma = s.sigma.cbegin();
  auto const points_to_symm_grad_v = s.symm_grad_v.cbegin();
  auto const points_to_rho_e_dot = s.rho_e_dot.begin();
  auto functor = [=] HPC_DEVICE (point_index const point) {
    auto const symm_grad_v = points_to_symm_grad_v[point].load();
    auto const sigma = points_to_sigma[point].load();
    auto const rho_e_dot = inner_product(sigma, symm_grad_v);
    points_to_rho_e_dot[point] = rho_e_dot;
  };
  hpc::for_each(hpc::device_policy(), s.points, functor);
}

HPC_NOINLINE inline void update_e(state& s, hpc::time<double> const dt,
    material_index const material,
    hpc::device_vector<hpc::specific_energy<double>, point_index> const& old_e_vector)
{
  auto const points_to_rho_e_dot = s.rho_e_dot.cbegin();
  auto const points_to_rho = s.rho.cbegin();
  auto const points_to_old_e = old_e_vector.cbegin();
  auto const points_to_e = s.e.begin();
  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    for (auto const point : elements_to_points[element]) {
      auto const rho_e_dot = points_to_rho_e_dot[point];
      auto const rho = points_to_rho[point];
      auto const e_dot = rho_e_dot / rho;
      auto const old_e = points_to_old_e[point];
      auto const e = old_e + dt * e_dot;
      points_to_e[point] = e;
    }
  };
  hpc::for_each(hpc::device_policy(), s.element_sets[material], functor);
}

HPC_NOINLINE inline void apply_viscosity(input const& in, state& s) {
  auto const points_to_symm_grad_v = s.symm_grad_v.cbegin();
  auto const elements_to_h_art = s.h_art.cbegin();
  auto const points_to_c = s.c.cbegin();
  auto const c1 = in.quadratic_artificial_viscosity;
  auto const c2 = in.linear_artificial_viscosity;
  auto const points_to_rho = s.rho.cbegin();
  auto const points_to_sigma = s.sigma.begin();
  auto const points_to_nu_art = s.nu_art.begin();
  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    auto const h_art = elements_to_h_art[element];
    for (auto const point : elements_to_points[element]) {
      auto const symm_grad_v = points_to_symm_grad_v[point].load();
      auto const div_v = trace(symm_grad_v);
      if (div_v >= 0.0) {
        points_to_nu_art[point] = 0.0;
      } else {
        auto const c = points_to_c[point];
        auto const nu_art = c1 * ((-div_v) * (h_art * h_art)) + c2 * c * h_art;
        points_to_nu_art[point] = nu_art;
        auto const rho = points_to_rho[point];
        auto const sigma_art = (rho * nu_art) * symm_grad_v;
        auto const sigma = points_to_sigma[point].load();
        auto const sigma_tilde = sigma + sigma_art;
        points_to_sigma[point] = sigma_tilde;
      }
    }
  };
  hpc::for_each(hpc::device_policy(), s.elements, functor);
}

HPC_NOINLINE inline void volume_average_J(state& s) {
  auto const points_to_V = s.V.cbegin();
  auto const points_to_F = s.F_total.begin();
  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    hpc::volume<double> total_V0 = 0.0;
    hpc::volume<double> total_V = 0.0;
    for (auto const point : elements_to_points[element]) {
      auto const F = points_to_F[point].load();
      auto const J = determinant(F);
      auto const V = points_to_V[point];
      auto const V0 = V / J;
      total_V0 += V0;
      total_V += V;
    }
    auto const average_J = total_V / total_V0;
    for (auto const point : elements_to_points[element]) {
      auto const old_F = points_to_F[point].load();
      auto const old_J = determinant(old_F);
      auto const new_F = cbrt(average_J / old_J) * old_F;
      points_to_F[point] = new_F;
    }
  };
  hpc::for_each(hpc::device_policy(), s.elements, functor);
}

HPC_NOINLINE inline void volume_average_rho(state& s) {
  auto const points_to_V = s.V.cbegin();
  auto const points_to_rho = s.rho.begin();
  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    hpc::mass<double> mass = 0.0;
    hpc::volume<double> total_V = 0.0;
    for (auto const point : elements_to_points[element]) {
      auto const rho = points_to_rho[point];
      auto const V = points_to_V[point];
      mass += V * rho;
      total_V += V;
    }
    auto const average_rho = mass / total_V;
    for (auto const point : elements_to_points[element]) {
      points_to_rho[point] = average_rho;
    }
  };
  hpc::for_each(hpc::device_policy(), s.elements, functor);
}

HPC_NOINLINE inline void volume_average_e(state& s) {
  auto const points_to_V = s.V.cbegin();
  auto const points_to_rho = s.rho.cbegin();
  auto const points_to_e = s.e.begin();
  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    hpc::energy<double> energy = 0.0;
    hpc::mass<double> mass = 0.0;
    for (auto const point : elements_to_points[element]) {
      auto const rho = points_to_rho[point];
      auto const e = points_to_e[point];
      auto const V = points_to_V[point];
      energy += V * (rho * e);
      mass += V * rho;
    }
    auto const average_e = energy / mass;
    for (auto const point : elements_to_points[element]) {
      points_to_e[point] = average_e;
    }
  };
  hpc::for_each(hpc::device_policy(), s.elements, functor);
}

HPC_NOINLINE inline void volume_average_p(state& s) {
  auto const points_to_V = s.V.cbegin();
  auto const points_to_sigma = s.sigma.begin();
  auto const elements_to_points = s.elements * s.points_in_element;
  auto functor = [=] HPC_DEVICE (element_index const element) {
    hpc::volume<double> total_V = 0.0;
    decltype(hpc::pressure<double>() * hpc::volume<double>()) p_integral = 0.0;
    for (auto const point : elements_to_points[element]) {
      auto const sigma = points_to_sigma[point].load();
      auto const p = -(1.0 / 3.0) * trace(sigma);
      auto const V = points_to_V[point];
      p_integral += V * p;
      total_V += V;
    }
    auto const average_p = p_integral / total_V;
    for (auto const point : elements_to_points[element]) {
      auto const old_sigma = points_to_sigma[point].load();
      auto const new_sigma = deviatoric_part(old_sigma) - average_p;
      points_to_sigma[point] = new_sigma;
    }
  };
  hpc::for_each(hpc::device_policy(), s.elements, functor);
}

HPC_DEVICE void variational_J2_point(hpc::deformation_gradient<double> const &F, j2::Properties const props,
    hpc::symmetric3x3<double> &sigma, double &Keff, double& Geff,
    double& potential, hpc::deformation_gradient<double> &Fp)
{
  auto const J = determinant(F);
  auto const Jm13 = 1.0 / std::cbrt(J);
  auto const Jm23 = Jm13 * Jm13;
  double const logJ = std::log(J);

  double const& K = props.K;
  double const& G = props.G;

  auto const We_vol = 0.5*K*logJ*logJ;
  auto const p = K*logJ/J;

  auto Fe_tr = F*hpc::inverse(Fp);
  auto dev_Ce_tr = Jm23*hpc::transpose(Fe_tr)*Fe_tr;
  auto dev_Ee_tr = 0.5*hpc::log(dev_Ce_tr);
  auto const dev_M_tr = 2.0*G*dev_Ee_tr;
  double const sigma_tr_eff = std::sqrt(1.5)*hpc::norm(dev_M_tr);
  auto Np{hpc::matrix3x3<double>::zero()};
  if (sigma_tr_eff > 0) {
    Np = 1.5*dev_M_tr/sigma_tr_eff;
  }

  double S = j2::FlowStrength(props, eqps);
  double const r0 = sigma_tr_eff - S;
  auto r = r0;

  double delta_eqps = 0;
  double const tolerance = 1e-10;
  if (r > tolerance) {
    constexpr int max_iters = 8;
    int iters = 0;
    bool converged = false;
    while (!converged) {
      if (iters == max_iters) break;
      double H = j2::HardeningRate(props, eqps + delta_eqps) + j2::ViscoplasticHardeningRate(props, delta_eqps, dt);
      double dr = -3.0*G - H;
      double corr = -r/dr;
      delta_eqps += corr;
      double S = j2::FlowStrength(props, eqps + delta_eqps) + j2::ViscoplasticStress(props, delta_eqps, dt);
      r = sigma_tr_eff - 3.0*G*delta_eqps - S;
      converged = std::abs(r/r0) < tolerance;
      ++iters;
    }
    if (!converged) {
      throw;
      // TODO: handle non-convergence error
    }
    auto dFp = hpc::exp(delta_eqps*Np);
    Fp = dFp*Fp;
    eqps += delta_eqps;
  }
  auto Ee_correction = delta_eqps*Np;
  auto dev_Ee = dev_Ee_tr - Ee_correction;
  auto dev_sigma = 1.0/J*hpc::transpose(hpc::inverse(Fe_tr))*(dev_M_tr - 2.0*G*Ee_correction)*hpc::transpose(Fe_tr);

  auto We_dev = G*hpc::inner_product(dev_Ee, dev_Ee);
  auto psi_star = j2::ViscoplasticDualKineticPotential(props, delta_eqps, dt);
  auto Wp = j2::HardeningPotential(props, eqps);

  sigma = hpc::symmetric3x3<double>(dev_sigma) + p*hpc::symmetric3x3<double>::identity();

  Keff = K;
  Geff = G;
  potential = We_vol + We_dev + Wp + psi_star;
}

HPC_NOINLINE inline void update_otm_material_state(input const& in, state& s, material_index const material,
    hpc::time<double> const) {
  auto const points_to_F_total = s.F_total.cbegin();
  auto const points_to_sigma = s.sigma.begin();
  auto const points_to_K = s.K.begin();
  auto const points_to_G = s.G.begin();
  auto const K = in.K0[material];
  auto const G = in.G0[material];
  auto dt = s.dt;
  auto const is_neo_hookean = in.enable_neo_Hookean[material];
  auto const is_sierra_J2 = in.enable_sierra_J2[material];
  auto functor = [=] HPC_DEVICE (point_index const point) {
      auto const F = points_to_F_total[point].load();
      auto&& sigma = points_to_sigma[point].load();
      auto&& Keff = points_to_K[point];
      auto&& Geff = points_to_G[point];
      double potential(0);
      if (is_neo_hookean == true) {
        neo_Hookean_point(F, K, G, sigma, Keff, Geff, potential);
      }
      if (is_sierra_J2 == true) {
        j2::Properties props{.K = K, .G = G, .S0 = 0,
                             .n = 1, .eps0 = 1.0,
                             .Svis0 = 0, .m = 1.0, .eps_dot0 = 1.0};
        auto Fp{hpc::matrix3x3<double>::identity()};
        double eqps = 0;
        variational_J2_point(F, props, dt, sigma, Keff, Geff, potential, Fp, eqps);
      }
  };
  hpc::for_each(hpc::device_policy(), s.points, functor);
}

>>>>>>> Variational J2 WIP
HPC_NOINLINE inline void update_single_material_state(input const& in, state& s, material_index const material,
    hpc::time<double> const dt,
    hpc::device_vector<hpc::pressure<double>, node_index> const& old_p_h) {
  auto const is_meshless = in.element == MESHLESS;
  if (is_meshless == true) {
    // TODO: Maybe don't need this here at all.
    // otm_update_material_state(in, s, material, dt);
  } else {
    if (in.enable_neo_Hookean[material]) {
      neo_Hookean(in, s, material);
    }
#if defined(HYPER_EP)
    else if (in.enable_hyper_ep[material]) {
      hyper_ep_update(in, s, material);
    }
#endif // HYPER_EP
    if (in.enable_ideal_gas[material]) {
      if (in.enable_nodal_energy[material]) {
        nodal_ideal_gas(in, s, material);
      } else {
        ideal_gas(in, s, material);
      }
    }
    if (in.enable_nodal_pressure[material] || in.enable_nodal_energy[material]) {
      if (in.enable_p_prime[material]) {
        update_sigma_with_p_h_p_prime(in, s, material, dt, old_p_h);
      } else {
        update_sigma_with_p_h(s, material);
      }
    }
  }
}

HPC_NOINLINE inline void update_material_state(input const& in, state& s,
    hpc::time<double> const dt,
    hpc::host_vector<hpc::device_vector<hpc::pressure<double>, node_index>, material_index> const& old_p_h) {
  hpc::fill(hpc::device_policy(), s.sigma, hpc::symmetric_stress<double>::zero());
  hpc::fill(hpc::device_policy(), s.G, hpc::pressure<double>(0.0));
  for (auto const material : in.materials) {
    update_single_material_state(in, s, material, dt, old_p_h[material]);
  }
}

HPC_NOINLINE inline void update_a_from_material_state(input const& in, state& s) {
  update_element_force(s);
  update_nodal_force(s);
  update_a(s);
  for (auto const& cond : in.zero_acceleration_conditions) {
    zero_acceleration(s.node_sets[cond.boundary], cond.axis, &s.a);
  }
}

HPC_NOINLINE inline void midpoint_predictor_corrector_step(input const& in, state& s) {
  hpc::fill(hpc::device_policy(), s.u, hpc::displacement<double>(0.0, 0.0, 0.0));
  hpc::device_array_vector<hpc::velocity<double>, node_index> old_v(s.nodes.size());
  hpc::copy(hpc::device_policy(), s.v, old_v);
  hpc::device_vector<hpc::specific_energy<double>, point_index> old_e(s.points.size());
  hpc::copy(hpc::device_policy(), s.e, old_e);
  hpc::host_vector<hpc::device_vector<hpc::pressure<double>, node_index>, material_index> old_p_h(in.materials.size());
  hpc::host_vector<hpc::device_vector<hpc::specific_energy<double>, node_index>, material_index> old_e_h(in.materials.size());
  for (auto const material : in.materials) {
    if (in.enable_nodal_pressure[material]) {
      old_p_h[material].resize(s.nodes.size());
      hpc::copy(hpc::device_policy(), s.p_h[material], old_p_h[material]);
    }
    if (in.enable_nodal_energy[material]) {
      if (in.enable_p_prime[material]) {
        old_p_h[material].resize(s.nodes.size());
        hpc::copy(hpc::device_policy(), s.p_h[material], old_p_h[material]);
      }
      old_e_h[material].resize(s.nodes.size());
      hpc::copy(hpc::device_policy(), s.e_h[material], old_e_h[material]);
    }
  }
  constexpr int npc = 2;
  for (int pc = 0; pc < npc; ++pc) {
    if (pc == 0) advance_time(in, s.max_stable_dt, s.next_file_output_time, &s.time, &s.dt);
    update_v(s, s.dt / 2.0, old_v);
    update_symm_grad_v(s);
    bool const last_pc = (pc == (npc - 1));
    auto const half_dt = last_pc ? s.dt : s.dt / 2.0;
    for (auto const material : in.materials) {
      if (in.enable_nodal_pressure[material]) {
        update_p_h(s, half_dt, material, old_p_h[material]);
      }
    }
    stress_power(s);
    for (auto const material : in.materials) {
      if (in.enable_nodal_energy[material]) {
        update_e_h_dot_from_a(in, s, material);
        update_e_h(s, half_dt, material, old_e_h[material]);
      } else {
        update_e(s, half_dt, material, old_e);
      }
    }
    if (in.enable_e_averaging) volume_average_e(s);
    update_u(s, half_dt);
    if (last_pc) {
      update_v(s, s.dt, old_v);
    }
    update_x(s);
    update_reference(s);
    if (in.enable_J_averaging) volume_average_J(s);
    if (in.enable_rho_averaging) volume_average_rho(s);
    for (auto const material : in.materials) {
      if (in.enable_nodal_energy[material]) {
        update_nodal_density(s, material);
        interpolate_rho(s, material);
      }
    }
    if (in.enable_adapt) {
      update_quality(in, s);
      update_min_quality(s);
    }
    update_symm_grad_v(s);
    update_h_min(in, s);
    if (in.enable_viscosity) update_h_art(in, s);
    update_material_state(in, s, half_dt, old_p_h);
    for (auto const material : in.materials) {
      if (in.enable_nodal_energy[material]) {
        interpolate_K(s, material);
      }
    }
    update_c(s);
    if (in.enable_viscosity) apply_viscosity(in, s);
    if (in.enable_p_averaging) volume_average_p(s);
    if (last_pc) update_element_dt(s);
    if (last_pc) find_max_stable_dt(s);
    update_a_from_material_state(in, s);
    for (auto const material : in.materials) {
      if (in.enable_nodal_pressure[material]) {
        update_p_h_dot_from_a(in, s, material);
      }
      if (!(in.enable_nodal_pressure[material] || in.enable_nodal_energy[material])) {
        update_p(s, material);
      }
    }
  }
}

HPC_NOINLINE inline void velocity_verlet_step(input const& in, state& s) {
  hpc::host_vector<hpc::device_vector<hpc::pressure<double>, node_index>, material_index> old_p_h(in.materials.size());
  advance_time(in, s.max_stable_dt, s.next_file_output_time, &s.time, &s.dt);
  update_v(s, s.dt / 2.0, s.v);
  hpc::fill(hpc::serial_policy(), s.u, hpc::displacement<double>(0.0, 0.0, 0.0));
  update_u(s, s.dt);
  update_x(s);
  update_reference(s);
  if (in.enable_J_averaging) volume_average_J(s);
  update_h_min(in, s);
  update_material_state(in, s, s.dt, old_p_h);
  update_c(s);
  update_element_dt(s);
  find_max_stable_dt(s);
  update_a_from_material_state(in, s);
  for (auto const material : in.materials) {
    if (in.enable_nodal_pressure[material]) {
      update_p_h_dot_from_a(in, s, material);
    } else {
      update_p(s, material);
    }
  }
  update_v(s, s.dt / 2.0, s.v);
}

HPC_NOINLINE inline void otm_explicit_step(input const& in, state& s) {
  hpc::host_vector<hpc::device_vector<hpc::pressure<double>, node_index>, material_index> old_p_h(in.materials.size());
  //auto const dt_prev = s.dt;
  advance_time(in, s.max_stable_dt, s.next_file_output_time, &s.time, &s.dt);
  auto const dt_curr = s.dt;
  hpc::fill(hpc::serial_policy(), s.u, hpc::displacement<double>(0.0, 0.0, 0.0));
  explicit_newmark_predict(s, dt_curr);
  update_x(s);
  update_reference(s);
  update_h_min(in, s);
  update_material_state(in, s, s.dt, old_p_h);
  update_c(s);
  update_element_dt(s);
  find_max_stable_dt(s);
  update_a_from_material_state(in, s);
  explicit_newmark_correct(s, dt_curr);
}

HPC_NOINLINE inline void time_integrator_step(input const& in, state& s) {
  switch (in.time_integrator) {
    case MIDPOINT_PREDICTOR_CORRECTOR:
      midpoint_predictor_corrector_step(in, s);
      break;
    case VELOCITY_VERLET:
      velocity_verlet_step(in, s);
      break;
    case OTM_EXPLICIT:
      otm_explicit_step(in, s);
      break;
  }
}

template <class Quantity>
HPC_NOINLINE inline void initialize_material_scalar(
    Quantity const scalar,
    state& s,
    material_index const material,
    hpc::device_vector<Quantity, point_index>& out) {
  auto const elements_to_points = s.elements * s.points_in_element;
  auto const points_to_scalar = out.begin();
  auto functor = [=] HPC_DEVICE (element_index const element) {
    for (auto const point : elements_to_points[element]) {
      points_to_scalar[point] = scalar;
    }
  };
  hpc::for_each(hpc::device_policy(), s.element_sets[material], functor);
}

HPC_NOINLINE inline void common_initialization_part1(input const& in, state& s) {
  initialize_V(in, s);
  if (in.enable_viscosity) update_h_art(in, s);
  update_nodal_mass(in, s);
  for (auto const material : in.materials) {
    if (in.enable_nodal_energy[material]) {
      update_nodal_density(s, material);
    }
  }
  initialize_grad_N(in, s);
  if (in.enable_adapt) {
    update_quality(in, s);
    update_min_quality(s);
  }
  update_symm_grad_v(s);
  update_h_min(in, s);
}

HPC_NOINLINE inline void common_initialization_part2(input const& in, state& s) {
  hpc::host_vector<hpc::device_vector<hpc::pressure<double>, node_index>, material_index> old_p_h(in.materials.size());
  if (hpc::any_of(hpc::serial_policy(), in.enable_p_prime)) {
    hpc::fill(hpc::device_policy(), s.element_dt, hpc::time<double>(0.0));
    hpc::fill(hpc::device_policy(), s.c, hpc::speed<double>(0.0));
  }
  update_material_state(in, s, 0.0, old_p_h);
  for (auto const material : in.materials) {
    if (in.enable_nodal_energy[material]) {
      interpolate_K(s, material);
    }
  }
  update_c(s);
  if (in.enable_viscosity) {
    apply_viscosity(in, s);
  } else {
    hpc::fill(hpc::device_policy(), s.nu_art, hpc::kinematic_viscosity<double>(0.0));
  }
  update_element_dt(s);
  find_max_stable_dt(s);
  update_a_from_material_state(in, s);
  for (auto const material : in.materials) {
    if (in.enable_nodal_pressure[material]) {
      update_p_h_dot_from_a(in, s, material);
    }
    if (!(in.enable_nodal_pressure[material] || in.enable_nodal_energy[material])) {
      update_p(s, material);
    }
    if (in.enable_nodal_energy[material]) {
      hpc::fill(hpc::device_policy(), s.q, hpc::heat_flux<double>::zero());
      if (in.enable_p_prime[material]) {
        hpc::fill(hpc::device_policy(), s.p_prime, hpc::pressure<double>(0));
      }
    }
  }
}

void run(input const& in) {
  std::cout << std::scientific << std::setprecision(17);
  auto const num_file_outputs = in.num_file_outputs;
  auto const file_output_period = num_file_outputs ? in.end_time / double(num_file_outputs) : hpc::time<double>(0.0);
  state s;
  build_mesh(in, s);
  if (in.x_transform) in.x_transform(&s.x);
  resize_state(in, s);
  assign_element_materials(in, s);
  compute_nodal_materials(in, s);
  collect_node_sets(in, s);
  collect_element_sets(in, s);
  for (auto const material : in.materials) {
    initialize_material_scalar(in.rho0[material], s, material, s.rho);
    if (in.enable_nodal_pressure[material]) {
      hpc::fill(hpc::device_policy(), s.p_h[material], double(0.0));
    }
    if (in.enable_nodal_energy[material]) {
      hpc::fill(hpc::device_policy(), s.e_h[material], in.e0[material]);
    } else {
      initialize_material_scalar(in.e0[material], s, material, s.e);
    }
  }
  assert(in.initial_v);
  in.initial_v(s.nodes, s.x, &s.v);
  hpc::fill(hpc::device_policy(), s.F_total, hpc::deformation_gradient<double>::identity());
  {
    hpc::fill(hpc::device_policy(), s.Fp_total, hpc::deformation_gradient<double>::identity());
    hpc::fill(hpc::device_policy(), s.temp, double(0.0));
    hpc::fill(hpc::device_policy(), s.ep, double(0.0));
    hpc::fill(hpc::device_policy(), s.ep_dot, double(0.0));
    hpc::fill(hpc::device_policy(), s.dp, double(0.0));
    hpc::fill(hpc::device_policy(), s.localized, int(0));
  }

  common_initialization_part1(in, s);
  common_initialization_part2(in, s);
  if (in.enable_adapt) initialize_h_adapt(s);
  file_writer output_file(in.name);
  s.next_file_output_time = num_file_outputs ? 0.0 : in.end_time;
  int file_output_index = 0;
  int file_period_index = 0;
  while (s.time < in.end_time) {
    if (num_file_outputs) {
      if (in.output_to_command_line) {
        std::cout << "outputting file n " << file_output_index << " time " << double(s.time) << "\n";
      }
      output_file.capture(in, s);
      output_file.write(in, file_output_index);
      ++file_output_index;
      ++file_period_index;
      s.next_file_output_time = double(file_period_index) * file_output_period;
      s.next_file_output_time = std::min(s.next_file_output_time, in.end_time);
    }
    while (s.time < s.next_file_output_time) {
      if (in.output_to_command_line) {
        std::cout << "step " << s.n << " time " << double(s.time) << " dt " << double(s.max_stable_dt) << "\n";
      }
      time_integrator_step(in, s);
      if (in.enable_adapt && (s.n % 10 == 0)) {
        for (int i = 0; i < 4; ++i) {
          adapt(in, s);
          resize_state(in, s);
          collect_element_sets(in, s);
          collect_node_sets(in, s);
          common_initialization_part1(in, s);
          common_initialization_part2(in, s);
        }
      }
      ++s.n;
    }
  }
  if (num_file_outputs) {
    if (in.output_to_command_line) {
      std::cout << "outputting last file n " << file_output_index << " time " << double(s.time) << "\n";
    }
    output_file.capture(in, s);
    output_file.write(in, file_output_index);
  }
  if (in.output_to_command_line) {
    std::cout << "final time " << double(s.time) << "\n";
  }
}

}

