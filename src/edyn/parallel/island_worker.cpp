#include "edyn/parallel/island_worker.hpp"
#include "edyn/collision/broadphase_worker.hpp"
#include "edyn/collision/contact_manifold.hpp"
#include "edyn/collision/contact_manifold_map.hpp"
#include "edyn/collision/narrowphase.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/config/config.h"
#include "edyn/parallel/job.hpp"
#include "edyn/comp/island.hpp"
#include "edyn/shapes/compound_shape.hpp"
#include "edyn/shapes/convex_mesh.hpp"
#include "edyn/shapes/polyhedron_shape.hpp"
#include "edyn/sys/update_aabbs.hpp"
#include "edyn/sys/update_inertias.hpp"
#include "edyn/sys/update_rotated_meshes.hpp"
#include "edyn/time/time.hpp"
#include "edyn/parallel/job_dispatcher.hpp"
#include "edyn/parallel/island_delta_builder.hpp"
#include "edyn/parallel/message.hpp"
#include "edyn/serialization/memory_archive.hpp"
#include "edyn/comp/dirty.hpp"
#include "edyn/comp/graph_node.hpp"
#include "edyn/comp/graph_edge.hpp"
#include "edyn/comp/rotated_mesh_list.hpp"
#include "edyn/math/constants.hpp"
#include "edyn/math/transform.hpp"
#include "edyn/collision/tree_view.hpp"
#include "edyn/util/aabb_util.hpp"
#include "edyn/util/rigidbody.hpp"
#include "edyn/util/vector.hpp"
#include "edyn/util/collision_util.hpp"
#include "edyn/context/settings.hpp"
#include <memory>
#include <variant>
#include <entt/entity/registry.hpp>

namespace edyn {

void island_worker_func(job::data_type &data) {
    auto archive = memory_input_archive(data.data(), data.size());
    intptr_t worker_intptr;
    archive(worker_intptr);
    auto *worker = reinterpret_cast<island_worker *>(worker_intptr);

    if (worker->is_terminating()) {
        // `worker` is dynamically allocated and must be manually deallocated
        // when it terminates.
        worker->do_terminate();
        delete worker;
    } else {
        worker->update();
    }
}

island_worker::island_worker(entt::entity island_entity, const settings &settings,
                             const material_mix_table &material_table,
                             message_queue_in_out message_queue)
    : m_message_queue(message_queue)
    , m_splitting(false)
    , m_state(state::init)
    , m_solver(m_registry)
    , m_delta_builder((*settings.make_island_delta_builder)())
    , m_importing_delta(false)
    , m_destroying_node(false)
    , m_topology_changed(false)
    , m_pending_split_calculation(false)
    , m_calculate_split_delay(0.6)
    , m_calculate_split_timestamp(0)
{
    m_registry.set<contact_manifold_map>(m_registry);
    m_registry.set<broadphase_worker>(m_registry);
    m_registry.set<narrowphase>(m_registry);
    m_registry.set<entity_graph>();
    m_registry.set<edyn::settings>(settings);
    m_registry.set<material_mix_table>(material_table);

    // Avoid multi-threading issues in the `should_collide` function by
    // pre-allocating the pools required in there.
    m_registry.prepare<collision_filter>();
    m_registry.prepare<collision_exclusion>();

    m_island_entity = m_registry.create();
    m_entity_map.insert(island_entity, m_island_entity);

    m_this_job.func = &island_worker_func;
    auto archive = fixed_memory_output_archive(m_this_job.data.data(), m_this_job.data.size());
    auto ctx_intptr = reinterpret_cast<intptr_t>(this);
    archive(ctx_intptr);
}

island_worker::~island_worker() = default;

void island_worker::init() {
    m_registry.on_construct<graph_node>().connect<&island_worker::on_construct_graph_node>(*this);
    m_registry.on_destroy<graph_node>().connect<&island_worker::on_destroy_graph_node>(*this);
    m_registry.on_destroy<graph_edge>().connect<&island_worker::on_destroy_graph_edge>(*this);
    m_registry.on_destroy<contact_manifold>().connect<&island_worker::on_destroy_contact_manifold>(*this);
    m_registry.on_construct<polyhedron_shape>().connect<&island_worker::on_construct_polyhedron_shape>(*this);
    m_registry.on_construct<compound_shape>().connect<&island_worker::on_construct_compound_shape>(*this);
    m_registry.on_destroy<rotated_mesh_list>().connect<&island_worker::on_destroy_rotated_mesh_list>(*this);

    m_message_queue.sink<island_delta>().connect<&island_worker::on_island_delta>(*this);
    m_message_queue.sink<msg::set_paused>().connect<&island_worker::on_set_paused>(*this);
    m_message_queue.sink<msg::step_simulation>().connect<&island_worker::on_step_simulation>(*this);
    m_message_queue.sink<msg::wake_up_island>().connect<&island_worker::on_wake_up_island>(*this);
    m_message_queue.sink<msg::set_com>().connect<&island_worker::on_set_com>(*this);
    m_message_queue.sink<msg::set_settings>().connect<&island_worker::on_set_settings>(*this);
    m_message_queue.sink<msg::set_material_table>().connect<&island_worker::on_set_material_table>(*this);

    // Process messages enqueued before the worker was started. This includes
    // the island deltas containing the initial entities that were added to
    // this island.
    process_messages();

    auto &settings = m_registry.ctx<edyn::settings>();
    if (settings.external_system_init) {
        (*settings.external_system_init)(m_registry);
    }

    // Run broadphase to initialize the internal dynamic trees with the
    // imported AABBs.
    auto &bphase = m_registry.ctx<broadphase_worker>();
    bphase.update();

    // Assign tree view containing the updated broad-phase tree.
    auto tview = bphase.view();
    m_registry.emplace<tree_view>(m_island_entity, tview);

    m_state = state::step;
}

void island_worker::on_destroy_contact_manifold(entt::registry &registry, entt::entity entity) {
    const auto importing = m_importing_delta;
    const auto splitting = m_splitting.load(std::memory_order_relaxed);

    // If importing, do not insert this event into the delta because the entity
    // was already destroyed in the coordinator.
    // If splitting, do not insert this destruction event into the delta because
    // the entity is not actually being destroyed, it's just being moved into
    // another island.
    if (!importing && !splitting) {
        m_delta_builder->destroyed(entity);
    }

    // Mapping might not yet exist if this entity was just created locally and
    // the coordinator has not yet replied back with the main entity id.
    if (m_entity_map.has_loc(entity)) {
        m_entity_map.erase_loc(entity);
    }
}

void island_worker::on_construct_graph_node(entt::registry &registry, entt::entity entity) {
    // It is possible that a new connected component appears in the graph when
    // a new node is created.
    m_topology_changed = true;
}

void island_worker::on_destroy_graph_node(entt::registry &registry, entt::entity entity) {
    auto &node = registry.get<graph_node>(entity);
    auto &graph = registry.ctx<entity_graph>();

    m_destroying_node = true;

    graph.visit_edges(node.node_index, [&] (entt::entity edge_entity) {
        registry.destroy(edge_entity);
    });

    m_destroying_node = false;

    graph.remove_all_edges(node.node_index);
    graph.remove_node(node.node_index);

    if (!m_importing_delta &&
        !m_splitting.load(std::memory_order_relaxed) &&
        !m_clearing_dangling_np_nodes) {
        m_delta_builder->destroyed(entity);
    }

    if (m_entity_map.has_loc(entity)) {
        m_entity_map.erase_loc(entity);
    }
}

void island_worker::on_destroy_graph_edge(entt::registry &registry, entt::entity entity) {
    auto &graph = registry.ctx<entity_graph>();

    auto &edge = registry.get<graph_edge>(entity);
    auto nodes = graph.edge_node_entities(edge.edge_index);

    for (auto node : std::array{nodes.first, nodes.second}) {
        if (!registry.any_of<procedural_tag>(node) && !m_possibly_dangling_np_nodes.contains(node)) {
            m_possibly_dangling_np_nodes.emplace(node);
        }
    }

    if (!m_destroying_node) {
        graph.remove_edge(edge.edge_index);
    }

    if (!m_importing_delta &&
        !m_splitting.load(std::memory_order_relaxed) &&
        !m_clearing_dangling_np_nodes) {
        m_delta_builder->destroyed(entity);
    }

    if (m_entity_map.has_loc(entity)) {
        m_entity_map.erase_loc(entity);
    }

    m_topology_changed = true;
}

void island_worker::on_construct_polyhedron_shape(entt::registry &registry, entt::entity entity) {
    m_new_polyhedron_shapes.push_back(entity);
}

void island_worker::on_construct_compound_shape(entt::registry &registry, entt::entity entity) {
    m_new_compound_shapes.push_back(entity);
}

void island_worker::on_destroy_rotated_mesh_list(entt::registry &registry, entt::entity entity) {
    auto &rotated = registry.get<rotated_mesh_list>(entity);
    if (rotated.next != entt::null) {
        // Cascade delete. Could lead to mega tall call stacks.
        registry.destroy(rotated.next);
    }
}

void island_worker::on_island_delta(const island_delta &delta) {
    // Import components from main registry.
    m_importing_delta = true;
    delta.import(m_registry, m_entity_map);

    for (auto remote_entity : delta.created_entities()) {
        if (!m_entity_map.has_rem(remote_entity)) continue;
        if (m_delta_builder->has_rem(remote_entity)) continue;
        auto local_entity = m_entity_map.remloc(remote_entity);
        m_delta_builder->insert_entity_mapping(remote_entity, local_entity);
    }

    auto &graph = m_registry.ctx<entity_graph>();
    auto node_view = m_registry.view<graph_node>();

    // Insert nodes in the graph for each rigid body.
    auto insert_node = [this] (entt::entity remote_entity, auto &) {
        insert_remote_node(remote_entity);
    };

    delta.created_for_each<dynamic_tag>(insert_node);
    delta.created_for_each<static_tag>(insert_node);
    delta.created_for_each<kinematic_tag>(insert_node);
    delta.created_for_each<external_tag>(insert_node);

    // Insert edges in the graph for constraints.
    delta.created_for_each(constraints_tuple, [&] (entt::entity remote_entity, const auto &con) {
        if (!m_entity_map.has_rem(remote_entity)) return;

        auto local_entity = m_entity_map.remloc(remote_entity);

        if (m_registry.any_of<graph_edge>(local_entity)) return;

        auto &node0 = node_view.get<graph_node>(con.body[0]);
        auto &node1 = node_view.get<graph_node>(con.body[1]);
        auto edge_index = graph.insert_edge(local_entity, node0.node_index, node1.node_index);
        m_registry.emplace<graph_edge>(local_entity, edge_index);
    });

    // When orientation is set manually, a few dependent components must be
    // updated, e.g. AABB, cached origin, inertia_world_inv, rotated meshes...
    delta.updated_for_each<orientation>([&] (entt::entity remote_entity, const orientation &orn) {
        if (!m_entity_map.has_rem(remote_entity)) return;

        auto local_entity = m_entity_map.remloc(remote_entity);

        if (auto *origin = m_registry.try_get<edyn::origin>(local_entity)) {
            auto &com = m_registry.get<center_of_mass>(local_entity);
            auto &pos = m_registry.get<position>(local_entity);
            *origin = to_world_space(-com, pos, orn);
        }

        if (m_registry.any_of<AABB>(local_entity)) {
            update_aabb(m_registry, local_entity);
        }

        if (m_registry.any_of<dynamic_tag>(local_entity)) {
            update_inertia(m_registry, local_entity);
        }

        if (m_registry.any_of<rotated_mesh_list>(local_entity)) {
            update_rotated_mesh(m_registry, local_entity);
        }
    });

    // When position is set manually, the AABB and cached origin must be updated.
    delta.updated_for_each<position>([&] (entt::entity remote_entity, const position &pos) {
        if (!m_entity_map.has_rem(remote_entity)) return;

        auto local_entity = m_entity_map.remloc(remote_entity);

        if (auto *origin = m_registry.try_get<edyn::origin>(local_entity)) {
            auto &com = m_registry.get<center_of_mass>(local_entity);
            auto &orn = m_registry.get<orientation>(local_entity);
            *origin = to_world_space(-com, pos, orn);
        }

        if (m_registry.any_of<AABB>(local_entity)) {
            update_aabb(m_registry, local_entity);
        }
    });

    m_importing_delta = false;
}

void island_worker::on_wake_up_island(const msg::wake_up_island &) {
    if (!m_registry.any_of<sleeping_tag>(m_island_entity)) return;

    auto builder = make_island_delta_builder(m_registry);

    auto &isle_timestamp = m_registry.get<island_timestamp>(m_island_entity);
    isle_timestamp.value = performance_time();
    builder->updated(m_island_entity, isle_timestamp);

    m_registry.view<sleeping_tag>().each([&] (entt::entity entity) {
        builder->destroyed<sleeping_tag>(entity);
    });
    m_registry.clear<sleeping_tag>();

    auto delta = builder->finish();
    m_message_queue.send<island_delta>(std::move(delta));
}

void island_worker::sync() {
    // Always update AABBs since they're needed for broad-phase in the coordinator.
    m_registry.view<AABB>().each([&] (entt::entity entity, AABB &aabb) {
        m_delta_builder->updated(entity, aabb);
    });

    // Updated contact points are needed when moving entities from one island to
    // another when merging/splitting in the coordinator.
    // TODO: the island worker refactor would eliminate the need to share these
    // components continuously.
    m_registry.view<contact_manifold>().each([&] (entt::entity entity, contact_manifold &manifold) {
        m_delta_builder->updated(entity, manifold);
    });

    // Update continuous components.
    auto &settings = m_registry.ctx<edyn::settings>();
    auto &index_source = *settings.index_source;
    m_registry.view<continuous>().each([&] (entt::entity entity, continuous &cont) {
        for (size_t i = 0; i < cont.size; ++i) {
            m_delta_builder->updated(entity, m_registry, index_source.type_id_of(cont.indices[i]));
        }
    });

    sync_dirty();

    auto delta = m_delta_builder->finish();
    m_message_queue.send<island_delta>(std::move(delta));
}

void island_worker::sync_dirty() {
    // Assign dirty components to the delta builder. This can be called at
    // any time to move the current dirty entities into the next island delta.
    m_registry.view<dirty>().each([&] (entt::entity entity, dirty &dirty) {
        if (dirty.is_new_entity) {
            m_delta_builder->created(entity);
        }

        m_delta_builder->created(entity, m_registry,
            dirty.created_indexes.begin(), dirty.created_indexes.end());
        m_delta_builder->updated(entity, m_registry,
            dirty.updated_indexes.begin(), dirty.updated_indexes.end());
        m_delta_builder->destroyed(entity,
            dirty.destroyed_indexes.begin(), dirty.destroyed_indexes.end());
    });

    m_registry.clear<dirty>();
}

void island_worker::clear_dangling_non_procedural_nodes() {
    m_clearing_dangling_np_nodes = true;

    auto &graph = m_registry.ctx<entity_graph>();
    auto node_view = m_registry.view<graph_node>();
    auto proc_view = m_registry.view<procedural_tag>();

    for (auto entity : m_possibly_dangling_np_nodes) {
        if (!m_registry.valid(entity)) {
            continue;
        }

        auto node_index = node_view.get<graph_node>(entity).node_index;
        bool has_procedural_neighbor = false;


        for (auto node_entity : m_registry.view<graph_node>()) {
            if (proc_view.contains(node_entity) &&
                graph.has_adjacency(node_index, node_view.get<graph_node>(node_entity).node_index)) {
                has_procedural_neighbor = true;
                break;
            }
        }

        if (!has_procedural_neighbor) {
            m_registry.destroy(entity);
        }
    }

    m_possibly_dangling_np_nodes.clear();
    m_clearing_dangling_np_nodes = false;
}

void island_worker::update() {
    switch (m_state) {
    case state::init:
        init();
        maybe_reschedule();
        break;
    case state::step:
        process_messages();

        if (should_step()) {
            begin_step();
            if (run_broadphase()) {
                if (run_narrowphase()) {
                    run_solver();
                    finish_step();
                    maybe_reschedule();
                }
            }
        } else {
            maybe_reschedule();
        }

        break;
    case state::begin_step:
        begin_step();
        reschedule_now();
        break;
    case state::solve:
        run_solver();
        finish_step();
        reschedule_now();
        break;
    case state::broadphase:
        if (run_broadphase()) {
            reschedule_now();
        }
        break;
    case state::broadphase_async:
        finish_broadphase();
        if (run_narrowphase()) {
            run_solver();
            finish_step();
            maybe_reschedule();
        }
        break;
    case state::narrowphase:
        if (run_narrowphase()) {
            run_solver();
            finish_step();
            maybe_reschedule();
        }
        break;
    case state::narrowphase_async:
        finish_narrowphase();
        run_solver();
        finish_step();
        maybe_reschedule();
        break;
    case state::finish_step:
        finish_step();
        maybe_reschedule();
        break;
    }
}

void island_worker::process_messages() {
    m_message_queue.update();
}

bool island_worker::should_step() {
    auto time = performance_time();

    if (m_state == state::begin_step) {
        m_step_start_time = time;
        return true;
    }

    auto &settings = m_registry.ctx<edyn::settings>();

    if (settings.paused || m_registry.any_of<sleeping_tag>(m_island_entity)) {
        return false;
    }

    auto &isle_time = m_registry.get<island_timestamp>(m_island_entity);
    auto dt = time - isle_time.value;

    if (dt < settings.fixed_dt) {
        return false;
    }

    m_step_start_time = time;
    m_state = state::begin_step;

    return true;
}

void island_worker::begin_step() {
    EDYN_ASSERT(m_state == state::begin_step);

    auto &settings = m_registry.ctx<edyn::settings>();
    if (settings.external_system_pre_step) {
        (*settings.external_system_pre_step)(m_registry);
    }

    // Initialize new shapes. Basically, create rotated meshes for new
    // imported polyhedron shapes.
    init_new_shapes();

    m_state = state::broadphase;
}

bool island_worker::run_broadphase() {
    EDYN_ASSERT(m_state == state::broadphase);
    auto &bphase = m_registry.ctx<broadphase_worker>();

    if (bphase.parallelizable()) {
        m_state = state::broadphase_async;
        bphase.update_async(m_this_job);
        return false;
    } else {
        bphase.update();
        m_state = state::narrowphase;
        return true;
    }
}

void island_worker::finish_broadphase() {
    EDYN_ASSERT(m_state == state::broadphase_async);
    auto &bphase = m_registry.ctx<broadphase_worker>();
    bphase.finish_async_update();
    m_state = state::narrowphase;
}

bool island_worker::run_narrowphase() {
    EDYN_ASSERT(m_state == state::narrowphase);
    auto &nphase = m_registry.ctx<narrowphase>();

    if (nphase.parallelizable()) {
        m_state = state::narrowphase_async;
        nphase.update_async(m_this_job);
        return false;
    } else {
        // Separating contact points will be destroyed in the next call. Move
        // the dirty contact points into the island delta before that happens
        // because the dirty component is removed as well, which would cause
        // points that were created in this step and are going to be destroyed
        // next to be missing in the island delta.
        sync_dirty();
        nphase.update();
        m_state = state::solve;
        return true;
    }
}

void island_worker::finish_narrowphase() {
    EDYN_ASSERT(m_state == state::narrowphase_async);
    // In the asynchronous narrow-phase update, separating contact points will
    // be destroyed in the next call. Following the same logic as above, move
    // the dirty contact points into the current island delta before that
    // happens.
    sync_dirty();
    auto &nphase = m_registry.ctx<narrowphase>();
    nphase.finish_async_update();
    m_state = state::solve;
}

void island_worker::run_solver() {
    EDYN_ASSERT(m_state == state::solve);
    m_solver.update(m_registry.ctx<edyn::settings>().fixed_dt);
    m_state = state::finish_step;
}

void island_worker::finish_step() {
    EDYN_ASSERT(m_state == state::finish_step);

    auto &isle_time = m_registry.get<island_timestamp>(m_island_entity);
    auto dt = m_step_start_time - isle_time.value;

    // Set a limit on the number of steps the worker can lag behind the current
    // time to prevent it from getting stuck in the past in case of a
    // substantial slowdown.
    auto &settings = m_registry.ctx<edyn::settings>();
    const auto fixed_dt = settings.fixed_dt;

    constexpr int max_lagging_steps = 10;
    auto num_steps = int(std::floor(dt / fixed_dt));

    if (num_steps > max_lagging_steps) {
        auto remainder = dt - num_steps * fixed_dt;
        isle_time.value = m_step_start_time - (remainder + max_lagging_steps * fixed_dt);
    } else {
        isle_time.value += fixed_dt;
    }

    m_delta_builder->updated<island_timestamp>(m_island_entity, isle_time);

    // Update tree view.
    auto &bphase = m_registry.ctx<broadphase_worker>();
    auto tview = bphase.view();
    m_registry.replace<tree_view>(m_island_entity, tview);
    m_delta_builder->updated(m_island_entity, tview);

    maybe_go_to_sleep();

    if (settings.external_system_post_step) {
        (*settings.external_system_post_step)(m_registry);
    }

    clear_dangling_non_procedural_nodes();
    sync();

    m_state = state::step;

    // Unfortunately, an island cannot be split immediately, because a merge could
    // happen at the same time in the coordinator, which might reference entities
    // that won't be present here anymore in the next update because they were moved
    // into another island which the coordinator could not be aware of at the
    // moment it was merging this island with another. Thus, this island sets its
    // splitting flag to true and sends the split request to the coordinator and it
    // is put to sleep until the coordinator calls `split()` which executes the
    // split and puts it back to run.
    if (should_split()) {
        m_splitting.store(true, std::memory_order_release);
        m_message_queue.send<msg::split_island>();
    }
}

bool island_worker::should_split() {
    if (!m_topology_changed) return false;

    auto time = performance_time();

    if (m_pending_split_calculation) {
        if (time - m_calculate_split_timestamp > m_calculate_split_delay) {
            m_pending_split_calculation = false;
            m_topology_changed = false;

            // If the graph has more than one connected component, it means
            // this island could be split.
            if (!m_registry.ctx<entity_graph>().is_single_connected_component()) {
                return true;
            }
        }
    } else {
        m_pending_split_calculation = true;
        m_calculate_split_timestamp = time;
    }

    return false;
}

void island_worker::reschedule_now() {
    job_dispatcher::global().async(m_this_job);
}

void island_worker::maybe_reschedule() {
    // Reschedule this job only if not paused nor sleeping nor splitting.
    if (m_splitting.load(std::memory_order_relaxed)) return;

    auto sleeping = m_registry.any_of<sleeping_tag>(m_island_entity);
    auto paused = m_registry.ctx<edyn::settings>().paused;

    // The update is done and this job can be rescheduled after this point
    auto reschedule_count = m_reschedule_counter.exchange(0, std::memory_order_acq_rel);
    EDYN_ASSERT(reschedule_count != 0);

    // If the number of reschedule requests is greater than one, it means there
    // are external requests involved, not just the normal internal reschedule.
    // Always reschedule for immediate execution in that case.
    if (reschedule_count == 1) {
        if (!paused && !sleeping) {
            reschedule_later();
        }
    } else {
        reschedule();
    }
}

void island_worker::reschedule_later() {
    // Only reschedule if it has not been scheduled and updated already.
    auto reschedule_count = m_reschedule_counter.fetch_add(1, std::memory_order_acq_rel);
    if (reschedule_count > 0) return;

    // If the timestamp of the current registry state is more that `m_fixed_dt`
    // before the current time, schedule it to run at a later time.
    auto time = performance_time();
    auto &isle_time = m_registry.get<island_timestamp>(m_island_entity);
    auto fixed_dt = m_registry.ctx<edyn::settings>().fixed_dt;
    auto delta_time = isle_time.value + fixed_dt - time;

    if (delta_time > 0) {
        job_dispatcher::global().async_after(delta_time, m_this_job);
    } else {
        job_dispatcher::global().async(m_this_job);
    }
}

void island_worker::reschedule() {
    // Do not reschedule if it's awaiting a split to be completed. The main
    // thread modifies the worker's registry during a split so this job must
    // not be run in parallel with that task.
    if (m_splitting.load(std::memory_order_relaxed)) return;

    // Only reschedule if it has not been scheduled and updated already.
    auto reschedule_count = m_reschedule_counter.fetch_add(1, std::memory_order_acq_rel);
    if (reschedule_count > 0) return;

    job_dispatcher::global().async(m_this_job);
}

void island_worker::init_new_shapes() {
    auto orn_view = m_registry.view<orientation>();
    auto polyhedron_view = m_registry.view<polyhedron_shape>();
    auto compound_view = m_registry.view<compound_shape>();

    for (auto entity : m_new_polyhedron_shapes) {
        if (!polyhedron_view.contains(entity)) continue;

        auto &polyhedron = polyhedron_view.get<polyhedron_shape>(entity);
        // A new `rotated_mesh` is assigned to it, replacing another reference
        // that could be already in there, thus preventing concurrent access.
        auto rotated = make_rotated_mesh(*polyhedron.mesh, orn_view.get<orientation>(entity));
        auto rotated_ptr = std::make_unique<rotated_mesh>(std::move(rotated));
        polyhedron.rotated = rotated_ptr.get();
        m_registry.emplace<rotated_mesh_list>(entity, polyhedron.mesh, std::move(rotated_ptr));
    }

    for (auto entity : m_new_compound_shapes) {
        if (!compound_view.contains(entity)) continue;

        auto &compound = compound_view.get<compound_shape>(entity);
        auto &orn = orn_view.get<orientation>(entity);
        auto prev_rotated_entity = entt::entity{entt::null};

        for (auto &node : compound.nodes) {
            if (!std::holds_alternative<polyhedron_shape>(node.shape_var)) continue;

            // Assign a `rotated_mesh_list` to this entity for the first
            // polyhedron and link it with more rotated meshes for the
            // remaining polyhedrons.
            auto &polyhedron = std::get<polyhedron_shape>(node.shape_var);
            auto local_orn = orn * node.orientation;
            auto rotated = make_rotated_mesh(*polyhedron.mesh, local_orn);
            auto rotated_ptr = std::make_unique<rotated_mesh>(std::move(rotated));
            polyhedron.rotated = rotated_ptr.get();

            if (prev_rotated_entity == entt::null) {
                m_registry.emplace<rotated_mesh_list>(entity, polyhedron.mesh, std::move(rotated_ptr), node.orientation);
                prev_rotated_entity = entity;
            } else {
                auto next = m_registry.create();
                m_registry.emplace<rotated_mesh_list>(next, polyhedron.mesh, std::move(rotated_ptr), node.orientation);

                auto &prev_rotated_list = m_registry.get<rotated_mesh_list>(prev_rotated_entity);
                prev_rotated_list.next = next;
                prev_rotated_entity = next;
            }
        }
    }

    m_new_polyhedron_shapes.clear();
    m_new_compound_shapes.clear();
}

void island_worker::insert_remote_node(entt::entity remote_entity) {
    if (!m_entity_map.has_rem(remote_entity)) return;

    auto local_entity = m_entity_map.remloc(remote_entity);
    auto non_connecting = !m_registry.any_of<procedural_tag>(local_entity);

    auto &graph = m_registry.ctx<entity_graph>();
    auto node_index = graph.insert_node(local_entity, non_connecting);
    m_registry.emplace<graph_node>(local_entity, node_index);
}

void island_worker::maybe_go_to_sleep() {
    if (could_go_to_sleep()) {
        const auto &isle_time = m_registry.get<island_timestamp>(m_island_entity);

        if (!m_sleep_timestamp) {
            m_sleep_timestamp = isle_time.value;
        } else {
            auto sleep_dt = isle_time.value - *m_sleep_timestamp;
            if (sleep_dt > island_time_to_sleep) {
                go_to_sleep();
                m_sleep_timestamp.reset();
            }
        }
    } else {
        m_sleep_timestamp.reset();
    }
}

bool island_worker::could_go_to_sleep() {
    // If any entity has a `sleeping_disabled_tag` then the island should
    // not go to sleep, since the movement of all entities depend on one
    // another in the same island.
    if (!m_registry.view<sleeping_disabled_tag>().empty()) {
        return false;
    }

    // Check if there are any entities moving faster than the sleep threshold.
    auto vel_view = m_registry.view<linvel, angvel, procedural_tag>();

    for (auto entity : vel_view) {
        auto [v, w] = vel_view.get<linvel, angvel>(entity);

        if ((length_sqr(v) > island_linear_sleep_threshold * island_linear_sleep_threshold) ||
            (length_sqr(w) > island_angular_sleep_threshold * island_angular_sleep_threshold)) {
            return false;
        }
    }

    return true;
}

void island_worker::go_to_sleep() {
    m_registry.emplace<sleeping_tag>(m_island_entity);
    m_delta_builder->created(m_island_entity, sleeping_tag{});

    // Assign `sleeping_tag` to all procedural entities.
    m_registry.view<procedural_tag>().each([&] (entt::entity entity) {
        if (auto *v = m_registry.try_get<linvel>(entity); v) {
            *v = vector3_zero;
            m_delta_builder->updated(entity, *v);
        }

        if (auto *w = m_registry.try_get<angvel>(entity); w) {
            *w = vector3_zero;
            m_delta_builder->updated(entity, *w);
        }

        m_registry.emplace<sleeping_tag>(entity);
        m_delta_builder->created(entity, sleeping_tag{});
    });
}

void island_worker::on_set_paused(const msg::set_paused &msg) {
    m_registry.ctx<edyn::settings>().paused = msg.paused;
    auto &isle_time = m_registry.get<island_timestamp>(m_island_entity);
    auto timestamp = performance_time();
    isle_time.value = timestamp;
}

void island_worker::on_step_simulation(const msg::step_simulation &) {
    if (!m_registry.any_of<sleeping_tag>(m_island_entity)) {
        m_state = state::begin_step;
    }
}

void island_worker::on_set_settings(const msg::set_settings &msg) {
    m_registry.ctx<settings>() = msg.settings;
}

void island_worker::on_set_material_table(const msg::set_material_table &msg) {
    m_registry.ctx<material_mix_table>() = msg.table;
}

void island_worker::on_set_com(const msg::set_com &msg) {
    auto entity = m_entity_map.remloc(msg.entity);
    apply_center_of_mass(m_registry, entity, msg.com);
}

entity_graph::connected_components_t island_worker::split() {
    EDYN_ASSERT(m_splitting.load(std::memory_order_relaxed));

    // Process any pending messages before splitting to ensure the registry
    // is up to date. This message usually would be a merge with another
    // island.
    process_messages();

    auto &graph = m_registry.ctx<entity_graph>();
    auto connected_components = graph.connected_components();

    if (connected_components.size() <= 1) {
        m_splitting.store(false, std::memory_order_release);
        reschedule_now();
        return {};
    }

    // Sort connected components by size. The biggest component will stay
    // in this island worker.
    std::sort(connected_components.begin(), connected_components.end(),
        [] (auto &lhs, auto &rhs) {
            auto lsize = lhs.nodes.size() + lhs.edges.size();
            auto rsize = rhs.nodes.size() + rhs.edges.size();
            return lsize > rsize;
        });

    // Collect non-procedural entities that remain in this island. Since
    // they can be present in multiple islands, it must not be removed
    // from this island in the next step.
    auto procedural_view = m_registry.view<procedural_tag>();
    auto &resident_connected_component = connected_components.front();
    std::vector<entt::entity> remaining_non_procedural_entities;

    for (auto entity : resident_connected_component.nodes) {
        if (!procedural_view.contains(entity)) {
            remaining_non_procedural_entities.push_back(entity);
        }
    }

    // Process connected components that are moving out of this island.
    // Update all components of all entities that are moving out in the current
    // island delta to ensure they're fully up to date in the coordinator and so
    // no data will be lost when firing up new island workers which will operate
    // on these entities.
    // Remove entities in the smaller connected components from this worker.
    // Non-procedural entities can be present in more than one connected component.
    // Do not remove entities that are still present in the biggest connected
    // component, thus skip the first.
    for (size_t i = 1; i < connected_components.size(); ++i) {
        auto &connected_component = connected_components[i];

        for (auto entity : connected_component.nodes) {
            if (!vector_contains(remaining_non_procedural_entities, entity) &&
                m_registry.valid(entity)) {
                m_delta_builder->updated_all(entity, m_registry);
                m_registry.destroy(entity);
            }
        }

        // All edges connecting to the destroyed nodes will be destroyed as well
        // in `on_destroy_graph_node()`.
    }

    // Refresh island tree view after nodes are removed and send it back to
    // the coordinator via the message queue.
    auto &bphase = m_registry.ctx<broadphase_worker>();
    auto tview = bphase.view();
    m_registry.replace<tree_view>(m_island_entity, tview);
    m_delta_builder->updated(m_island_entity, tview);
    auto delta = m_delta_builder->finish();
    m_message_queue.send<island_delta>(std::move(delta));

    m_splitting.store(false, std::memory_order_release);
    reschedule_now();

    return connected_components;
}

bool island_worker::is_terminated() const {
    return m_terminated.load(std::memory_order_acquire);
}

bool island_worker::is_terminating() const {
    return m_terminating.load(std::memory_order_acquire);
}

void island_worker::terminate() {
    m_splitting.store(false, std::memory_order_release); // Cancel split.
    m_terminating.store(true, std::memory_order_release);
    reschedule();
}

void island_worker::do_terminate() {
    {
        auto lock = std::lock_guard(m_terminate_mutex);
        m_terminated.store(true, std::memory_order_release);
    }
    m_terminate_cv.notify_one();
}

void island_worker::join() {
    auto lock = std::unique_lock(m_terminate_mutex);
    m_terminate_cv.wait(lock, [&] { return is_terminated(); });
}

}
