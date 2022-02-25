#include "edyn/networking/sys/client_side.hpp"
#include "edyn/collision/contact_manifold.hpp"
#include "edyn/collision/contact_manifold_map.hpp"
#include "edyn/comp/island.hpp"
#include "edyn/config/config.h"
#include "edyn/constraints/constraint.hpp"
#include "edyn/edyn.hpp"
#include "edyn/parallel/entity_graph.hpp"
#include "edyn/comp/graph_edge.hpp"
#include "edyn/comp/graph_node.hpp"
#include "edyn/networking/comp/entity_owner.hpp"
#include "edyn/networking/comp/networked_comp.hpp"
#include "edyn/networking/packet/general_snapshot.hpp"
#include "edyn/networking/packet/transient_snapshot.hpp"
#include "edyn/networking/packet/entity_request.hpp"
#include "edyn/networking/packet/util/pool_snapshot.hpp"
#include "edyn/networking/context/client_network_context.hpp"
#include "edyn/comp/dirty.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/parallel/job_dispatcher.hpp"
#include "edyn/parallel/merge/merge_component.hpp"
#include "edyn/networking/extrapolation_job.hpp"
#include "edyn/networking/util/non_proc_comp_state_history.hpp"
#include "edyn/time/time.hpp"
#include "edyn/util/island_util.hpp"
#include <entt/entity/fwd.hpp>
#include <entt/entity/registry.hpp>
#include <set>

namespace edyn {

void on_construct_networked_entity(entt::registry &registry, entt::entity entity) {
    auto &ctx = registry.ctx<client_network_context>();

    if (!ctx.importing_entities) {
        ctx.created_entities.push_back(entity);
    }
}

void on_destroy_networked_entity(entt::registry &registry, entt::entity entity) {
    auto &ctx = registry.ctx<client_network_context>();

    if (!ctx.importing_entities) {
        ctx.destroyed_entities.push_back(entity);

        if (ctx.entity_map.has_loc(entity)) {
            ctx.entity_map.erase_loc(entity);
        }
    }
}

void on_construct_entity_owner(entt::registry &registry, entt::entity entity) {
    auto &ctx = registry.ctx<client_network_context>();
    auto &owner = registry.get<entity_owner>(entity);

    if (owner.client_entity == ctx.client_entity) {
        ctx.owned_entities.emplace(entity);
    }
}

void on_destroy_entity_owner(entt::registry &registry, entt::entity entity) {
    auto &ctx = registry.ctx<client_network_context>();
    auto &owner = registry.get<entity_owner>(entity);

    if (owner.client_entity == ctx.client_entity) {
        ctx.owned_entities.erase(entity);
    }
}

static void update_input_history(entt::registry &registry, double timestamp) {
    // Insert input components into history only for entities owned by the
    // local client.
    auto &settings = registry.ctx<edyn::settings>();
    auto builder = (*settings.make_island_delta_builder)();
    auto &ctx = registry.ctx<client_network_context>();

    for (auto entity : ctx.owned_entities) {
        ctx.pool_snapshot_importer->insert_local_input_to_builder(registry, entity, *builder);
    }

    if (!builder->empty()) {
        ctx.state_history.emplace(builder->finish(), timestamp);
    }
}

void init_network_client(entt::registry &registry) {
    registry.set<client_network_context>();

    registry.on_construct<networked_tag>().connect<&on_construct_networked_entity>();
    registry.on_destroy<networked_tag>().connect<&on_destroy_networked_entity>();
    registry.on_construct<entity_owner>().connect<&on_construct_entity_owner>();
    registry.on_destroy<entity_owner>().connect<&on_destroy_entity_owner>();

    auto &settings = registry.ctx<edyn::settings>();
    settings.network_settings = client_network_settings{};
}

void deinit_network_client(entt::registry &registry) {
    registry.unset<client_network_context>();

    registry.on_construct<networked_tag>().disconnect<&on_construct_networked_entity>();
    registry.on_destroy<networked_tag>().disconnect<&on_destroy_networked_entity>();
    registry.on_construct<entity_owner>().disconnect<&on_construct_entity_owner>();
    registry.on_destroy<entity_owner>().disconnect<&on_destroy_entity_owner>();

    auto &settings = registry.ctx<edyn::settings>();
    settings.network_settings = {};
}

static void process_created_networked_entities(entt::registry &registry) {
    auto &ctx = registry.ctx<client_network_context>();

    if (ctx.created_entities.empty()) {
        return;
    }

    packet::create_entity packet;
    packet.entities = ctx.created_entities;

    for (auto entity : ctx.created_entities) {
        ctx.pool_snapshot_exporter->export_all(registry, entity, packet.pools);
        registry.emplace<entity_owner>(entity, ctx.client_entity);
    }

    // Sort components to ensure order of construction.
    std::sort(packet.pools.begin(), packet.pools.end(), [] (auto &&lhs, auto &&rhs) {
        return lhs.component_index < rhs.component_index;
    });

    ctx.packet_signal.publish(packet::edyn_packet{std::move(packet)});
    ctx.created_entities.clear();
}

static void process_destroyed_networked_entities(entt::registry &registry) {
    auto &ctx = registry.ctx<client_network_context>();

    if (ctx.destroyed_entities.empty()) {
        return;
    }

    packet::destroy_entity packet;
    packet.entities = std::move(ctx.destroyed_entities);
    ctx.packet_signal.publish(packet::edyn_packet{std::move(packet)});
}

static void maybe_publish_transient_snapshot(entt::registry &registry, double time) {
    auto &ctx = registry.ctx<client_network_context>();
    auto &settings = registry.ctx<edyn::settings>();
    auto &client_settings = std::get<client_network_settings>(settings.network_settings);

    if (time - ctx.last_snapshot_time < 1 / client_settings.snapshot_rate) {
        return;
    }

    ctx.last_snapshot_time = time;

    // Include transient components of all entities in the islands that contain
    // an entity owned by this client, excluding entities that are owned by
    // other clients.
    auto packet = packet::transient_snapshot{};

    auto island_entities = collect_islands_from_residents(registry, ctx.owned_entities.begin(), ctx.owned_entities.end());
    auto island_view = registry.view<island>();
    auto networked_view = registry.view<networked_tag>();
    auto owner_view = registry.view<entity_owner>();
    auto manifold_view = registry.view<contact_manifold>();

    auto export_transient = [&] (entt::entity entity) {
        auto is_owned_by_another_client =
            owner_view.contains(entity) &&
            std::get<0>(owner_view.get(entity)).client_entity != ctx.client_entity;

        if (networked_view.contains(entity) && !is_owned_by_another_client) {
            ctx.pool_snapshot_exporter->export_transient(registry, entity, packet.pools);
        }
    };

    for (auto island_entity : island_entities) {
        auto [island] = island_view.get(island_entity);

        for (auto entity : island.nodes) {
            export_transient(entity);
        }

        for (auto entity : island.edges) {
            if (manifold_view.contains(entity)) {
                packet.manifolds.push_back(std::get<0>(manifold_view.get(entity)));
            } else {
                export_transient(entity);
            }
        }
    }

    if (!packet.pools.empty()) {
        ctx.packet_signal.publish(packet::edyn_packet{std::move(packet)});
    }
}

static void apply_extrapolation_result(entt::registry &registry, extrapolation_result &result) {
    // Entities could've been destroyed while extrapolation was running.
    auto invalid_it = std::remove_if(result.entities.begin(), result.entities.end(),
                                     [&] (auto entity) { return !registry.valid(entity); });
    result.entities.erase(invalid_it, result.entities.end());

    auto island_entities = collect_islands_from_residents(registry, result.entities.begin(), result.entities.end());
    EDYN_ASSERT(!island_entities.empty());
    auto &coordinator = registry.ctx<island_coordinator>();

    for (auto island_entity : island_entities) {
        coordinator.send_island_message<extrapolation_result>(island_entity, result);
        coordinator.wake_up_island(island_entity);
    }

    if (result.terminated_early) {
        auto &ctx = registry.ctx<client_network_context>();
        ctx.extrapolation_timeout_signal.publish();
    }
}

static void process_finished_extrapolation_jobs(entt::registry &registry) {
    auto &ctx = registry.ctx<client_network_context>();

    // Check if extrapolation jobs are finished and merge their results into
    // the main registry.
    auto remove_it = std::remove_if(ctx.extrapolation_jobs.begin(), ctx.extrapolation_jobs.end(),
                                    [&] (extrapolation_job_context &extr_ctx) {
        if (extr_ctx.job->is_finished()) {
            auto &result = extr_ctx.job->get_result();
            apply_extrapolation_result(registry, result);
            return true;
        }
        return false;
    });
    ctx.extrapolation_jobs.erase(remove_it, ctx.extrapolation_jobs.end());
}

static void publish_dirty_components(entt::registry &registry) {
    // Share dirty networked entities using a general snapshot.
    auto dirty_view = registry.view<dirty, networked_tag>();

    if (dirty_view.size_hint() == 0) {
        return;
    }

    auto &ctx = registry.ctx<client_network_context>();
    auto packet = packet::general_snapshot{};

    for (auto [entity, dirty] : dirty_view.each()) {
        for (auto id : dirty.updated_indexes) {
            ctx.pool_snapshot_exporter->export_by_type_id(registry, entity, id, packet.pools);
        }
    }

    if (!packet.pools.empty()) {
        ctx.packet_signal.publish(packet::edyn_packet{std::move(packet)});
    }
}

static void merge_network_dirty_into_dirty(entt::registry &registry) {
    auto dirty_view = registry.view<dirty>();

    // Insert components marked as dirty during snapshot import into the regular
    // dirty component. This is done separately to avoid having the components
    // marked as dirty during snapshot import being sent back to the server
    // in `publish_dirty_components`.
    for (auto [entity, network_dirty] : registry.view<network_dirty>().each()) {
        if (!dirty_view.contains(entity)) {
            registry.emplace<dirty>(entity);
        }

        dirty_view.get<edyn::dirty>(entity).merge(network_dirty);
    }

    registry.clear<network_dirty>();
}

void update_network_client(entt::registry &registry) {
    auto time = performance_time();

    process_created_networked_entities(registry);
    process_destroyed_networked_entities(registry);
    maybe_publish_transient_snapshot(registry, time);
    process_finished_extrapolation_jobs(registry);
    update_input_history(registry, time);
    publish_dirty_components(registry);
    merge_network_dirty_into_dirty(registry);
}

static void process_packet(entt::registry &registry, const packet::client_created &packet) {
    auto &ctx = registry.ctx<client_network_context>();
    ctx.importing_entities = true;

    auto remote_entity = packet.client_entity;
    auto local_entity = registry.create();
    edyn::tag_external_entity(registry, local_entity, false);

    EDYN_ASSERT(ctx.client_entity == entt::null);
    ctx.client_entity = local_entity;
    ctx.client_entity_assigned_signal.publish();
    ctx.entity_map.insert(remote_entity, local_entity);

    auto emap_packet = packet::update_entity_map{};
    emap_packet.pairs.emplace_back(remote_entity, local_entity);
    ctx.packet_signal.publish(packet::edyn_packet{std::move(emap_packet)});

    ctx.importing_entities = false;
}

static void process_packet(entt::registry &registry, const packet::update_entity_map &emap) {
    auto &ctx = registry.ctx<client_network_context>();

    for (auto &pair : emap.pairs) {
        auto local_entity = pair.first;
        auto remote_entity = pair.second;
        ctx.entity_map.insert(remote_entity, local_entity);
    }
}

static void process_packet(entt::registry &registry, const packet::entity_request &req) {

}

static void process_packet(entt::registry &registry, const packet::entity_response &res) {
    auto &ctx = registry.ctx<client_network_context>();
    ctx.importing_entities = true;

    auto emap_packet = packet::update_entity_map{};

    for (auto remote_entity : res.entities) {
        if (ctx.entity_map.has_rem(remote_entity)) {
            continue;
        }

        auto local_entity = registry.create();
        ctx.entity_map.insert(remote_entity, local_entity);
        emap_packet.pairs.emplace_back(remote_entity, local_entity);
    }

    for (auto &pool : res.pools) {
        ctx.pool_snapshot_importer->import(registry, ctx.entity_map, pool);
    }

    for (auto remote_entity : res.entities) {
        auto local_entity = ctx.entity_map.remloc(remote_entity);
        registry.emplace<networked_tag>(local_entity);
    }

    ctx.importing_entities = false;

    if (!emap_packet.pairs.empty()) {
        ctx.packet_signal.publish(packet::edyn_packet{std::move(emap_packet)});
    }
}

template<typename T>
void create_graph_edge(entt::registry &registry, entt::entity entity) {
    if (registry.any_of<graph_edge>(entity)) return;

    auto &comp = registry.get<T>(entity);
    auto node_index0 = registry.get<graph_node>(comp.body[0]).node_index;
    auto node_index1 = registry.get<graph_node>(comp.body[1]).node_index;
    auto edge_index = registry.ctx<entity_graph>().insert_edge(entity, node_index0, node_index1);
    registry.emplace<graph_edge>(entity, edge_index);
}

template<typename... Ts>
void maybe_create_graph_edge(entt::registry &registry, entt::entity entity, [[maybe_unused]] std::tuple<Ts...>) {
    ((registry.any_of<Ts>(entity) ? create_graph_edge<Ts>(registry, entity) : void(0)), ...);
}

static void process_packet(entt::registry &registry, const packet::create_entity &packet) {
    auto &ctx = registry.ctx<client_network_context>();
    ctx.importing_entities = true;

    // Collect new entity mappings to send back to server.
    auto emap_packet = packet::update_entity_map{};

    // Create entities first...
    for (auto remote_entity : packet.entities) {
        if (ctx.entity_map.has_rem(remote_entity)) continue;

        auto local_entity = registry.create();
        ctx.entity_map.insert(remote_entity, local_entity);
        emap_packet.pairs.emplace_back(remote_entity, local_entity);
    }

    if (!emap_packet.pairs.empty()) {
        ctx.packet_signal.publish(packet::edyn_packet{std::move(emap_packet)});
    }

    // ... assign components later so that entity references will be available
    // to be mapped into the local registry.
    for (auto &pool : packet.pools) {
        ctx.pool_snapshot_importer->import(registry, ctx.entity_map, pool);
    }

    for (auto remote_entity : packet.entities) {
        auto local_entity = ctx.entity_map.remloc(remote_entity);

        if (!registry.all_of<networked_tag>(local_entity)) {
            registry.emplace<networked_tag>(local_entity);
        }
    }

    // Create nodes and edges in entity graph.
    for (auto remote_entity : packet.entities) {
        auto local_entity = ctx.entity_map.remloc(remote_entity);

        if (registry.any_of<rigidbody_tag, external_tag>(local_entity) &&
            !registry.all_of<graph_node>(local_entity)) {
            auto non_connecting = !registry.any_of<procedural_tag>(local_entity);
            auto node_index = registry.ctx<entity_graph>().insert_node(local_entity, non_connecting);
            registry.emplace<graph_node>(local_entity, node_index);
        }

        if (registry.any_of<rigidbody_tag, procedural_tag>(local_entity)) {
            registry.emplace<discontinuity>(local_entity);
        }
    }

    for (auto remote_entity : packet.entities) {
        auto local_entity = ctx.entity_map.remloc(remote_entity);
        maybe_create_graph_edge(registry, local_entity, constraints_tuple);
    }

    ctx.importing_entities = false;
}

static void process_packet(entt::registry &registry, const packet::destroy_entity &packet) {
    auto &ctx = registry.ctx<client_network_context>();
    ctx.importing_entities = true;

    for (auto remote_entity : packet.entities) {
        if (!ctx.entity_map.has_rem(remote_entity)) continue;

        auto local_entity = ctx.entity_map.remloc(remote_entity);
        ctx.entity_map.erase_rem(remote_entity);

        if (registry.valid(local_entity)) {
            registry.destroy(local_entity);
        }
    }

    ctx.importing_entities = false;
}

static void collect_unknown_entities(const entt::registry &registry, entity_map &entity_map,
                                     const std::vector<entt::entity> &remote_entities,
                                     entt::sparse_set &unknown_entities) {
    // Find remote entities that have no local counterpart.
    for (auto remote_entity : remote_entities) {
        if (entity_map.has_rem(remote_entity)) {
            auto local_entity = entity_map.remloc(remote_entity);

            // In the unusual situation where an existing mapping is an invalid
            // entity, erase it from the entity map and consider it unknown.
            if (!registry.valid(local_entity)) {
                entity_map.erase_loc(local_entity);

                if (!unknown_entities.contains(remote_entity)) {
                    unknown_entities.emplace(remote_entity);
                }
            }
        } else if (!unknown_entities.contains(remote_entity)) {
            unknown_entities.emplace(remote_entity);
        }
    }
}

static bool request_unknown_entities_in_pools(entt::registry &registry,
                                              const std::vector<pool_snapshot> &pools) {
    auto &ctx = registry.ctx<client_network_context>();
    entt::sparse_set unknown_entities;

    for (auto &pool : pools) {
        collect_unknown_entities(registry, ctx.entity_map, pool.ptr->get_entities(), unknown_entities);
    }

    if (!unknown_entities.empty()) {
        // Request unknown entities.
        // TODO: prevent the same entity from being requested repeatedly.
        auto req = packet::entity_request{};
        req.entities.insert(req.entities.end(), unknown_entities.begin(), unknown_entities.end());
        ctx.packet_signal.publish(packet::edyn_packet{std::move(req)});
        return true;
    }

    return false;
}

static void insert_input_to_state_history(entt::registry &registry, const std::vector<pool_snapshot> &pools, double time) {
    auto &settings = registry.ctx<edyn::settings>();
    auto builder = (*settings.make_island_delta_builder)();

    auto &ctx = registry.ctx<client_network_context>();
    ctx.pool_snapshot_importer->insert_remote_input_to_builder(registry, pools, ctx.entity_map, *builder);

    if (!builder->empty()) {
        ctx.state_history.emplace(builder->finish(), time);
    }
}

static void snap_to_transient_snapshot(entt::registry &registry, const packet::transient_snapshot &snapshot) {
    auto &ctx = registry.ctx<client_network_context>();

    auto snapshot_local = snapshot;
    snapshot_local.convert_remloc(ctx.entity_map);

    // Collect all entities present in snapshot and find islands where they
    // reside and finally send the snapshot to the island workers.
    auto entities = snapshot_local.get_entities();
    auto island_entities = collect_islands_from_residents(registry, entities.begin(), entities.end());
    EDYN_ASSERT(!island_entities.empty());
    auto &coordinator = registry.ctx<island_coordinator>();

    for (auto island_entity : island_entities) {
        coordinator.send_island_message<packet::transient_snapshot>(island_entity, snapshot_local);
        coordinator.wake_up_island(island_entity);
    }
}

static void process_packet(entt::registry &registry, const packet::transient_snapshot &snapshot) {
    auto contains_unknown_entities = request_unknown_entities_in_pools(registry, snapshot.pools);

    auto &ctx = registry.ctx<client_network_context>();
    auto &settings = registry.ctx<edyn::settings>();
    auto &client_settings = std::get<client_network_settings>(settings.network_settings);

    const auto time = performance_time();
    const double snapshot_time = time - (ctx.server_playout_delay + client_settings.round_trip_time / 2);

    // Input from other clients must be always added to the state history.
    // The server won't send input components of entities owned by this client.
    insert_input_to_state_history(registry, snapshot.pools, snapshot_time);

    // If extrapolation is not enabled send the snapshot directly to the
    // island workers. They will snap to this state and add the differences
    // to the discontinuity components.
    if (!client_settings.extrapolation_enabled) {
        snap_to_transient_snapshot(registry, snapshot);
        return;
    }

    if (contains_unknown_entities) {
        // Do not perform extrapolation if it contains unknown entities as the
        // result would not make much sense if all parts are not involved. Wait
        // until the entity request is completed and then extrapolations will
        // be performed normally again. This should not happen very often.
        return;
    }

    // Ignore it if the number of current extrapolation jobs is at maximum.
    if (ctx.extrapolation_jobs.size() >= client_settings.max_concurrent_extrapolations) {
        return;
    }

    // Translate transient snapshot into client's space so entities in the
    // snapshot will make sense in this registry. This is particularly
    // important for the extrapolation job, or else it won't be able to
    // assimilate the server-side entities with client side-entities.
    auto snapshot_local = snapshot;
    snapshot_local.convert_remloc(ctx.entity_map);

    // Collect all entities to be included in extrapolation, that is, basically
    // all entities in the transient snapshot packet and the edges connecting
    // them.
    auto snapshot_entities = snapshot_local.get_entities();
    auto entities = entt::sparse_set{};
    auto node_view = registry.view<graph_node>();
    auto &graph = registry.ctx<entity_graph>();

    for (auto entity : snapshot_entities) {
        entities.emplace(entity);

        if (node_view.contains(entity)) {
            auto node_index = node_view.get<graph_node>(entity).node_index;

            graph.visit_edges(node_index, [&] (auto edge_index) {
                auto edge_entities = graph.edge_node_entities(edge_index);
                auto other_entity = edge_entities.first == entity ? edge_entities.second : edge_entities.first;

                if (snapshot_entities.contains(other_entity)) {
                    auto edge_entity = graph.edge_entity(edge_index);

                    if (!entities.contains(edge_entity)) {
                        entities.emplace(edge_entity);
                    }
                }
            });
        }
    }

    // TODO: only include the necessary static entities.
    for (auto entity : registry.view<static_tag>()) {
        if (!entities.contains(entity)) {
            entities.emplace(entity);
        }
    }

    // Create registry snapshot to send to extrapolation job.
    extrapolation_input input;
    input.extrapolation_component_pool_import_by_id_func = ctx.extrapolation_component_pool_import_by_id_func;
    input.is_input_component_func = ctx.is_input_component_func;
    (*ctx.extrapolation_component_pool_import_func)(input.pools, registry, entities);
    input.start_time = snapshot_time;

    for (auto entity : entities) {
        if (auto *owner = registry.try_get<entity_owner>(entity);
            owner && owner->client_entity == ctx.client_entity)
        {
            input.owned_entities.emplace(entity);
        }
    }

    input.entities = std::move(entities);
    input.transient_snapshot = std::move(snapshot_local);

    // Create extrapolation job and put the registry snapshot and the transient
    // snapshot into its message queue.
    auto &material_table = registry.ctx<material_mix_table>();

    auto job = std::make_unique<extrapolation_job>(std::move(input), settings, material_table, ctx.state_history);
    job->reschedule();

    ctx.extrapolation_jobs.push_back(extrapolation_job_context{std::move(job)});
}

static void process_packet(entt::registry &registry, const packet::general_snapshot &snapshot) {
    const auto time = performance_time();
    auto &settings = registry.ctx<edyn::settings>();
    auto &client_settings = std::get<client_network_settings>(settings.network_settings);
    auto &ctx = registry.ctx<client_network_context>();
    const double snapshot_time = time - (ctx.server_playout_delay + client_settings.round_trip_time / 2);

    insert_input_to_state_history(registry, snapshot.pools, snapshot_time);

    request_unknown_entities_in_pools(registry, snapshot.pools);

    for (auto &pool : snapshot.pools) {
        ctx.pool_snapshot_importer->import(registry, ctx.entity_map, pool);
    }
}

static void process_packet(entt::registry &registry, const packet::set_playout_delay &delay) {
    auto &ctx = registry.ctx<client_network_context>();
    ctx.server_playout_delay = delay.value;
}

void client_handle_packet(entt::registry &registry, const packet::edyn_packet &packet) {
    std::visit([&] (auto &&inner_packet) {
        process_packet(registry, inner_packet);
    }, packet.var);
}

entt::sink<void()> on_client_entity_assigned(entt::registry &registry) {
    auto &ctx = registry.ctx<client_network_context>();
    return entt::sink{ctx.client_entity_assigned_signal};
}

bool client_owns_entity(const entt::registry &registry, entt::entity entity) {
    auto &ctx = registry.ctx<client_network_context>();
    return ctx.client_entity == registry.get<entity_owner>(entity).client_entity;
}

}
