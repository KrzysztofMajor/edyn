#ifndef EDYN_NETWORKING_SETTINGS_CLIENT_NETWORKING_SETTINGS_HPP
#define EDYN_NETWORKING_SETTINGS_CLIENT_NETWORKING_SETTINGS_HPP

#include "edyn/math/scalar.hpp"
namespace edyn {

struct client_networking_settings {
    double snapshot_rate {30};
    double round_trip_time {0};
    bool extrapolation_enabled {true};
    unsigned max_concurrent_extrapolations {2};

    // The discontinuity error will be multiplied by this value after every
    // step. That means this value is sensitive to the fixed delta time since
    // a lower delta time means higher step rate, thus faster decay.
    scalar discontinuity_decay_rate {scalar(0.8)};
};

}

#endif // EDYN_NETWORKING_SETTINGS_CLIENT_NETWORKING_SETTINGS_HPP
