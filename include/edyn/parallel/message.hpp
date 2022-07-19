#ifndef EDYN_PARALLEL_MESSAGE_HPP
#define EDYN_PARALLEL_MESSAGE_HPP

#include <entt/entity/fwd.hpp>
#include "edyn/math/vector3.hpp"
#include "edyn/context/settings.hpp"
#include "edyn/dynamics/material_mixing.hpp"
#include "edyn/networking/util/pool_snapshot.hpp"
#include "edyn/parallel/message_dispatcher.hpp"
#include "edyn/util/registry_operation.hpp"

namespace edyn::msg {

struct set_paused {
    bool paused;
};

struct set_settings {
    edyn::settings settings;
};

struct set_material_table {
    edyn::material_mix_table table;
};

struct step_simulation {};

struct set_com {
    entt::entity entity;
    vector3 com;
};

/**
 * Message sent by worker to coordinator after every step of the simulation
 * containing everything that changed since the previous update.
 */
struct step_update {
    registry_operation_collection ops;
    double timestamp;
};

/**
 * Message sent by the coordinator to a worker asking entities and components
 * to be created, destroyed or updated.
 */
struct update_entities {
    registry_operation_collection ops;
};

struct apply_network_pools {
    std::vector<entt::entity> entities;
    std::vector<pool_snapshot> pools;
};

}

#endif // EDYN_PARALLEL_MESSAGE_HPP
