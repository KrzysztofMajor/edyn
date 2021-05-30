#ifndef EDYN_CONSTRAINTS_CONSTRAINT_HPP
#define EDYN_CONSTRAINTS_CONSTRAINT_HPP

#include <tuple>
#include <entt/fwd.hpp>
#include "edyn/constraints/distance_constraint.hpp"
#include "edyn/constraints/soft_distance_constraint.hpp"
#include "edyn/constraints/point_constraint.hpp"
#include "edyn/constraints/contact_constraint.hpp"
#include "edyn/constraints/hinge_constraint.hpp"
#include "edyn/constraints/generic_constraint.hpp"
#include "edyn/constraints/null_constraint.hpp"
#include "edyn/constraints/gravity_constraint.hpp"
#include "edyn/dynamics/row_cache.hpp"
#include "edyn/constraints/prepare_constraints.hpp"

namespace edyn {

/**
 * @brief Tuple of all available constraints. They are solved in this order so
 * the more important constraints should be the last in the list.
 */
static const auto constraints_tuple = std::tuple<
    null_constraint,
    gravity_constraint,
    point_constraint,
    distance_constraint,
    soft_distance_constraint,
    hinge_constraint,
    generic_constraint,
    contact_constraint
>{};

inline
void prepare_constraints(entt::registry &registry, row_cache &cache, scalar dt) {
    std::apply([&] (auto ... c) {
        (prepare_constraints<decltype(c)>(registry, cache, dt), ...);
    }, constraints_tuple);
}

inline
void iterate_constraints(entt::registry &registry, row_cache &cache, scalar dt) {
    std::apply([&] (auto ... c) {
        (iterate_constraints<decltype(c)>(registry, cache, dt), ...);
    }, constraints_tuple);
}

}

#endif // EDYN_CONSTRAINTS_CONSTRAINT_HPP