#ifndef EDYN_CONSTRAINTS_DISTANCE_CONSTRAINT
#define EDYN_CONSTRAINTS_DISTANCE_CONSTRAINT

#include <array>
#include <entt/entity/fwd.hpp>
#include "edyn/math/vector3.hpp"
#include "edyn/constraints/constraint_base.hpp"
#include "edyn/constraints/prepare_constraints.hpp"

namespace edyn {

struct distance_constraint : public constraint_base {
    std::array<vector3, 2> pivot;
    scalar distance {0};
    scalar impulse {0};
};

template<typename Archive>
void serialize(Archive &archive, distance_constraint &c) {
    archive(c.body);
    archive(c.pivot);
    archive(c.distance);
    archive(c.impulse);
}

template<>
void prepare_constraint<distance_constraint>(distance_constraint &con, row_cache_sparse::entry &cache_entry, scalar dt,
                        const vector3 &originA, const vector3 &posA, const quaternion &ornA,
                        const vector3 &linvelA, const vector3 &angvelA,
                        scalar inv_mA, const matrix3x3 &inv_IA, delta_linvel &dvA, delta_angvel &dwA,
                        const vector3 &originB, const vector3 &posB, const quaternion &ornB,
                        const vector3 &linvelB, const vector3 &angvelB,
                        scalar inv_mB, const matrix3x3 &inv_IB, delta_linvel &dvB, delta_angvel &dwB);

}

#endif // EDYN_CONSTRAINTS_DISTANCE_CONSTRAINT
