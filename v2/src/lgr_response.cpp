#include <lgr_response.hpp>

namespace lgr {

Response::Response(Simulation& sim_in, Omega_h::InputMap& pl) : sim(sim_in) {
  when.reset(setup_when(pl));
}

void Response::out_of_line_virtual_method() {}

}  // namespace lgr
