#include "edyn/build_settings.h"
#include "comp/shared_comp.hpp"
#include "comp/dirty.hpp"
#include "comp/graph_node.hpp"
#include "comp/graph_edge.hpp"
#include "math/constants.hpp"
#include "math/scalar.hpp"
#include "math/vector3.hpp"
#include "math/vector2.hpp"
#include "math/quaternion.hpp"
#include "math/matrix3x3.hpp"
#include "math/math.hpp"
#include "math/geom.hpp"
#include "time/time.hpp"
#include "util/rigidbody.hpp"
#include "util/constraint_util.hpp"
#include "util/shape_util.hpp"
#include "util/shape_volume.hpp"
#include "util/tuple_util.hpp"
#include "util/entity_set.hpp"
#include "collision/contact_manifold.hpp"
#include "collision/contact_point.hpp"
#include "shapes/create_paged_triangle_mesh.hpp"
#include "serialization/s11n.hpp"
#include "parallel/job_dispatcher.hpp"
#include "parallel/parallel_for.hpp"
#include "parallel/parallel_for_async.hpp"
#include "parallel/message_queue.hpp"
#include "parallel/island_delta_builder.hpp"
#include "util/moment_of_inertia.hpp"
#include "collision/contact_manifold_map.hpp"

namespace edyn {

/**
 * @brief Initializes Edyn's internals such as its thread pool and job system.
 * Call it before using Edyn.
 */
void init();

/**
 * @brief Undoes what was done by `init()`. Call it when Edyn is not needed anymore.
 */
void deinit();

/**
 * @brief Attaches Edyn to an EnTT registry.
 * @param registry The registry to be setup to run Edyn.
 */
void attach(entt::registry &registry);

/**
 * @brief Detaches Edyn from an EnTT registry.
 * @param registry The registry to be freed from Edyn's context.
 */
void detach(entt::registry &registry);

/**
 * @brief Get the fixed simulation delta time for each step.
 * @param registry Data source.
 * @return Fixed delta time in seconds.
 */
scalar get_fixed_dt(entt::registry &registry);

/**
 * @brief Set the fixed simulation delta time for each step.
 * @param registry Data source.
 * @param dt Delta time in seconds.
 */
void set_fixed_dt(entt::registry &registry, scalar dt);

/**
 * @brief Checks if simulation is paused.
 * @param registry Data source.
 * @return Whether simulation is paused.
 */
bool is_paused(entt::registry &registry);

/**
 * @brief Pauses simulation.
 * @param registry Data source.
 */
void set_paused(entt::registry &registry, bool paused);

/**
 * @brief Updates the simulation. Call it regularly.
 * The actual physics simulation runs in other threads. This function only
 * does coordination of background simulation jobs. It's expected to be a
 * lightweight call.
 * @param registry Data source.
 */
void update(entt::registry &registry);

/**
 * @brief Runs a single step for a paused simulation.
 * @param registry Data source.
 */
void step_simulation(entt::registry &registry);

/**
 * @brief Propagates changes to a component to the island worker where the
 * entity currently resides.
 * @tparam Component Component type.
 * @param registry Data source.
 * @param entity The entity that owns the component.
 */
template<typename... Component>
void refresh(entt::registry &registry, entt::entity entity);

/**
 * @brief Checks whether there is a contact manifold connecting the two entities.
 * @param registry Data source.
 * @param entities Pair of entities.
 * @return Whether a contact manifold exists between the two entities.
 */
bool manifold_exists(entt::registry &registry, entity_pair entities);

/**
 * @brief Get contact manifold entity for a pair of entities.
 * @param registry Data source.
 * @param entities Pair of entities.
 * @return Contact manifold entity.
 */
entt::entity get_manifold_entity(entt::registry &registry, entity_pair entities);

}
