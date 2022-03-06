#include "edyn/networking/sys/server_side.hpp"
#include "edyn/comp/island.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/networking/packet/client_created.hpp"
#include "edyn/networking/packet/general_snapshot.hpp"
#include "edyn/networking/packet/transient_snapshot.hpp"
#include "edyn/networking/packet/update_entity_map.hpp"
#include "edyn/networking/packet/util/pool_snapshot.hpp"
#include "edyn/networking/comp/remote_client.hpp"
#include "edyn/networking/comp/aabb_of_interest.hpp"
#include "edyn/networking/comp/entity_owner.hpp"
#include "edyn/networking/sys/update_aabbs_of_interest.hpp"
#include "edyn/networking/context/server_network_context.hpp"
#include "edyn/parallel/merge/merge_component.hpp"
#include "edyn/parallel/message.hpp"
#include "edyn/time/time.hpp"
#include "edyn/util/entity_map.hpp"
#include "edyn/edyn.hpp"
#include "edyn/util/island_util.hpp"
#include "edyn/util/vector.hpp"
#include <entt/entity/registry.hpp>
#include <algorithm>
#include <set>

namespace edyn {

static void update_island_entity_owners(entt::registry &registry) {
    // The client has ownership of their entities if they're the only client in
    // the island where the entity resides. They're also granted temporary
    // ownership of all other entities in that island.
    auto owner_view = registry.view<entity_owner>();

    for (auto [island_entity, island, island_owner] : registry.view<island, entity_owner>().each()) {
        // Set island owner to null and find out whether it can have a single owner.
        island_owner.client_entity = entt::null;

        for (auto it = island.nodes.begin(); it != island.edges.end(); ++it) {
            if (it == island.nodes.end()) {
                it = island.edges.begin();
            }

            auto entity = *it;

            if (!owner_view.contains(entity)) {
                continue;
            }

            auto [owner] = owner_view.get(entity);

            if (owner.client_entity == entt::null) {
                continue;
            }

            if (island_owner.client_entity == entt::null) {
                // Island is not owned by any client yet, thus assign this
                // client as the owner.
                island_owner.client_entity = owner.client_entity;
            } else if (island_owner.client_entity != owner.client_entity) {
                // Island contains more than one client in it, thus it cannot
                // be owned by either.
                island_owner.client_entity = entt::null;
                break;
            }
        }
    }
}

bool is_fully_owned_by_client(const entt::registry &registry, entt::entity client_entity, entt::entity entity) {
    auto owner_view = registry.view<entity_owner>();

    if (auto *resident = registry.try_get<island_resident>(entity)) {
        auto [island_owner] = owner_view.get(resident->island_entity);
        return island_owner.client_entity == client_entity;
    } else if (auto *resident = registry.try_get<multi_island_resident>(entity)) {
        for (auto island_entity : resident->island_entities) {
            auto [island_owner] = owner_view.get(island_entity);

            if (island_owner.client_entity != client_entity) {
                return false;
            }
        }

        return true;
    }

    return true;
}

static void process_packet(entt::registry &registry, entt::entity client_entity, const packet::entity_request &req) {
    entt::sparse_set valid_entities;

    for (auto entity : req.entities) {
        if (registry.valid(entity) && !valid_entities.contains(entity)) {
            valid_entities.emplace(entity);
        }
    }

    // Send back all entities in all islands associated with the request
    // entities. This is necessary because otherwise the response could be
    // incomplete, e.g. dependent entities won't be sent which will result
    // in an import failure on the other end.
    auto island_entities = collect_islands_from_residents(registry, valid_entities.begin(), valid_entities.end());
    auto island_view = registry.view<island>();
    auto networked_view = registry.view<networked_tag>();
    entt::sparse_set all_entities;

    for (auto island_entity : island_entities) {
        auto [island] = island_view.get(island_entity);

        for (auto entity : island.nodes) {
            if (!all_entities.contains(entity) && networked_view.contains(entity)) {
                all_entities.emplace(entity);
            }
        }

        for (auto entity : island.edges) {
            if (!all_entities.contains(entity) && networked_view.contains(entity)) {
                all_entities.emplace(entity);
            }
        }
    }

    // Include client entities of all entity owners.
    entt::sparse_set client_entities;
    auto owner_view = registry.view<entity_owner>();

    for (auto entity : all_entities) {
        if (owner_view.contains(entity)) {
            auto [owner] = owner_view.get(entity);

            if (!client_entities.contains(owner.client_entity)) {
                client_entities.emplace(owner.client_entity);
            }
        }
    }

    auto &ctx = registry.ctx<server_network_context>();
    auto res = packet::entity_response{};
    res.entities.insert(res.entities.end(), all_entities.begin(), all_entities.end());
    res.entities.insert(res.entities.end(), client_entities.begin(), client_entities.end());

    for (auto entity : res.entities) {
        ctx.pool_snapshot_exporter->export_all(registry, entity, res.pools);
    }

    if (!res.entities.empty()) {
        std::sort(res.pools.begin(), res.pools.end(), [] (auto &&lhs, auto &&rhs) {
            return lhs.component_index < rhs.component_index;
        });

        auto &client = registry.get<remote_client>(client_entity);
        client.packet_signal.publish(client_entity, packet::edyn_packet{res});
    }
}

static void process_packet(entt::registry &registry, entt::entity client_entity, const packet::entity_response &res) {
    // TODO: import requested entities.
}

static void process_packet(entt::registry &registry, entt::entity client_entity, packet::transient_snapshot &snapshot) {
    auto &ctx = registry.ctx<server_network_context>();
    const bool check_ownership = true;
    const bool mark_dirty = false;

    // Transform snapshot entities into local registry space.
    for (auto &pool : snapshot.pools) {
        ctx.pool_snapshot_importer->transform_to_local(registry, client_entity, pool, check_ownership);

        // If this pool holds input components, import them directly into
        // the main registry.
        ctx.pool_snapshot_importer->import_input_local(registry, client_entity, pool, mark_dirty);
    }

    // Get islands of all entities contained in transient snapshot and send the
    // snapshot to them. They will import the pre-processed state into their
    // registries. Later, these components will be updated in the main registry
    // via a registry snapshot.
    auto entities = snapshot.get_entities();
    auto island_entities = collect_islands_from_residents(registry, entities.begin(), entities.end());
    auto &coordinator = registry.ctx<island_coordinator>();
    auto msg = msg::apply_network_pools{std::move(snapshot.pools)};

    for (auto island_entity : island_entities) {
        coordinator.send_island_message<msg::apply_network_pools>(island_entity, msg);
        coordinator.wake_up_island(island_entity);
    }
}

static void process_packet(entt::registry &registry, entt::entity client_entity, const packet::general_snapshot &snapshot) {
    auto &ctx = registry.ctx<server_network_context>();
    const bool check_ownership = true;
    const bool mark_dirty = true;

    for (auto &pool : snapshot.pools) {
        ctx.pool_snapshot_importer->import(registry, client_entity, pool, check_ownership, mark_dirty);
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

static void process_packet(entt::registry &registry, entt::entity client_entity, const packet::create_entity &packet) {
    auto &ctx = registry.ctx<server_network_context>();
    auto &client = registry.get<remote_client>(client_entity);

    // Collect entity mappings for new entities to send back to client.
    auto emap_packet = packet::update_entity_map{};
    emap_packet.timestamp = performance_time();

    // Create entities first import pools later since components might contain
    // entities which have to be mapped from remote to local.
    for (auto remote_entity : packet.entities) {
        if (client.entity_map.has_rem(remote_entity)) continue;

        auto local_entity = registry.create();
        registry.emplace<entity_owner>(local_entity, client_entity);

        emap_packet.pairs.emplace_back(remote_entity, local_entity);
        client.entity_map.insert(remote_entity, local_entity);
        client.owned_entities.push_back(local_entity);
    }

    if (!emap_packet.pairs.empty()) {
        client.packet_signal.publish(client_entity, packet::edyn_packet{emap_packet});
    }

    // Must not check ownership because entities are being created for the the
    // client thus all entities are already assumed to be owned by the client.
    // Also, checking ownership at this point would fail since nodes and edges
    // haven't yet been created and islands haven't been assigned.
    const bool check_ownership = false;

    // Do not mark components as dirty because they will already be sent with
    // the new entities to island workers thus marking them as dirty would
    // cause them to be created twice in the worker.
    const bool mark_dirty = false;

    for (auto &pool : packet.pools) {
        ctx.pool_snapshot_importer->import(registry, client_entity, pool, check_ownership, mark_dirty);
    }

    // Create nodes and edges in entity graph and assign networked tags.
    for (auto remote_entity : packet.entities) {
        auto local_entity = client.entity_map.remloc(remote_entity);

        if (!registry.all_of<networked_tag>(local_entity)) {
            registry.emplace<networked_tag>(local_entity);
        }

        if (registry.any_of<rigidbody_tag, external_tag>(local_entity) &&
            !registry.all_of<graph_node>(local_entity)) {
            auto non_connecting = !registry.any_of<procedural_tag>(local_entity);
            auto node_index = registry.ctx<entity_graph>().insert_node(local_entity, non_connecting);
            registry.emplace<graph_node>(local_entity, node_index);
        } else {
            // If it's not a node, it might be an edge.
            for (auto remote_entity : packet.entities) {
                auto local_entity = client.entity_map.remloc(remote_entity);
                maybe_create_graph_edge(registry, local_entity, constraints_tuple);
            }
        }
    }
}

static void process_packet(entt::registry &registry, entt::entity client_entity, const packet::destroy_entity &packet) {
    auto &client = registry.get<remote_client>(client_entity);
    auto &aabboi = registry.get<aabb_of_interest>(client_entity);

    for (auto remote_entity : packet.entities) {
        if (client.entity_map.has_rem(remote_entity)) {
            auto local_entity = client.entity_map.remloc(remote_entity);

            if (registry.valid(local_entity)) {
                auto *owner = registry.try_get<entity_owner>(local_entity);

                if (owner && owner->client_entity == client_entity) {
                    registry.destroy(local_entity);
                    client.entity_map.erase_rem(remote_entity);
                    vector_erase(client.owned_entities, local_entity);

                    // Remove from AABB of interest of owner to prevent notifying
                    // the requester itself of destruction of these entities.
                    if (aabboi.entities.contains(local_entity)) {
                        aabboi.entities.erase(local_entity);
                    }
                }
            }
        }
    }
}

static void process_packet(entt::registry &registry, entt::entity client_entity, const packet::update_entity_map &packet) {
    auto &client = registry.get<remote_client>(client_entity);

    for (auto &pair : packet.pairs) {
        auto local_entity = pair.first;
        auto remote_entity = pair.second;
        client.entity_map.insert(remote_entity, local_entity);
    }
}

static void process_packet(entt::registry &, entt::entity, const packet::client_created &) {}
static void process_packet(entt::registry &, entt::entity, const packet::set_playout_delay &) {}

static void process_packet(entt::registry &registry, entt::entity client_entity, const packet::time_request &req) {
    auto res = packet::time_response{req.id, performance_time()};
    auto &client = registry.get<remote_client>(client_entity);
    client.packet_signal.publish(client_entity, packet::edyn_packet{res});
}

static void process_packet(entt::registry &registry, entt::entity client_entity, const packet::time_response &res) {
    auto &client = registry.get<remote_client>(client_entity);

    clock_sync_process_time_response(client.clock_sync, res, [&] (packet::edyn_packet &&packet) {
        client.packet_signal.publish(client_entity, packet);
    });
}

void init_network_server(entt::registry &registry) {
    registry.set<server_network_context>();
    // Assign an entity owner to every island created.
    registry.on_construct<island>().connect<&entt::registry::emplace<entity_owner>>();

    auto &settings = registry.ctx<edyn::settings>();
    settings.network_settings = server_network_settings{};
}

void deinit_network_server(entt::registry &registry) {
    registry.unset<server_network_context>();
    registry.on_construct<island>().disconnect<&entt::registry::emplace<entity_owner>>();

    auto &settings = registry.ctx<edyn::settings>();
    settings.network_settings = {};
}

static void server_process_timed_packets(entt::registry &registry, double time) {
    registry.view<remote_client>().each([&] (entt::entity client_entity, remote_client &client) {
        auto it = client.packet_queue.begin();

        for (; it != client.packet_queue.end(); ++it) {
            if (it->timestamp > time) {
                break;
            }

            std::visit([&] (auto &&packet) {
                using PacketType = std::decay_t<decltype(packet)>;

                if constexpr(has_type<PacketType, packet::timed_packets_tuple_t>::value) {
                    process_packet(registry, client_entity, packet);
                }
            }, it->packet.var);
        }

        client.packet_queue.erase(client.packet_queue.begin(), it);
    });
}

static void publish_pending_created_clients(entt::registry &registry) {
    auto &ctx = registry.ctx<server_network_context>();

    for (auto client_entity : ctx.pending_created_clients) {
        auto &client = registry.get<remote_client>(client_entity);
        auto packet = packet::client_created{client_entity};
        client.packet_signal.publish(client_entity, packet::edyn_packet{packet});
    }

    ctx.pending_created_clients.clear();
}

static void publish_client_current_snapshots(entt::registry &registry) {
    // Send out accumulated changes to clients.
    registry.view<remote_client>().each([&] (entt::entity client_entity, remote_client &client) {
        if (client.current_snapshot.pools.empty()) return;
        auto packet = packet::edyn_packet{std::move(client.current_snapshot)};
        client.packet_signal.publish(client_entity, packet);
        EDYN_ASSERT(client.current_snapshot.pools.empty());
    });
}

static void process_aabb_of_interest_destroyed_entities(entt::registry &registry,
                                                        entt::entity client_entity,
                                                        remote_client &client,
                                                        aabb_of_interest &aabboi,
                                                        double time) {
    if (aabboi.destroy_entities.empty()) {
        return;
    }

    // Notify client of entities that have been removed from its AABB-of-interest.
    auto owner_view = registry.view<entity_owner>();
    auto packet = packet::destroy_entity{};
    packet.timestamp = time;

    for (auto entity : aabboi.destroy_entities) {
        // Ignore entities owned by client.
        if (!registry.valid(entity) ||
            !owner_view.contains(entity) ||
            std::get<0>(owner_view.get(entity)).client_entity != client_entity)
        {
            packet.entities.push_back(entity);

            // Must not forget to remove entity from client's entity map. Would be
            // a problem later when this entity comes back into the AABB-of-interest,
            // which would cause a new entity mapping to be created, which would lead
            // to an assertion failure since a mapping would already exist.
            if (client.entity_map.has_loc(entity)) {
                client.entity_map.erase_loc(entity);
            }
        }
    }

    aabboi.destroy_entities.clear();

    if (!packet.entities.empty()) {
        client.packet_signal.publish(client_entity, packet::edyn_packet{packet});
    }
}

static void process_aabb_of_interest_created_entities(entt::registry &registry,
                                                      entt::entity client_entity,
                                                      remote_client &client,
                                                      aabb_of_interest &aabboi,
                                                      double time) {
    if (aabboi.create_entities.empty()) {
        return;
    }

    auto owner_view = registry.view<entity_owner>();
    auto packet = packet::create_entity{};
    packet.timestamp = time;

    for (auto entity : aabboi.create_entities) {
        // Ignore entities owned by client, since these entities must be
        // persistent in the client-side.
        if (!owner_view.contains(entity) ||
            std::get<0>(owner_view.get(entity)).client_entity != client_entity)
        {
            packet.entities.push_back(entity);
        }
    }

    if (!packet.entities.empty()) {
        auto &ctx = registry.ctx<server_network_context>();

        for (auto entity : packet.entities) {
            ctx.pool_snapshot_exporter->export_all(registry, entity, packet.pools);
        }

        // Sort components to ensure order of construction on the other end.
        std::sort(packet.pools.begin(), packet.pools.end(), [] (auto &&lhs, auto &&rhs) {
            return lhs.component_index < rhs.component_index;
        });

        client.packet_signal.publish(client_entity, packet::edyn_packet{packet});
    }

    aabboi.create_entities.clear();
}

static void maybe_publish_client_transient_snapshot(entt::registry &registry,
                                                    entt::entity client_entity,
                                                    remote_client &client,
                                                    aabb_of_interest &aabboi,
                                                    double time) {
    if (time - client.last_snapshot_time < 1 / client.snapshot_rate) {
        return;
    }

    client.last_snapshot_time = time;

    auto &ctx = registry.ctx<server_network_context>();
    auto packet = packet::transient_snapshot{};
    packet.timestamp = time;

    for (auto entity : aabboi.entities) {
        if (registry.any_of<sleeping_tag, static_tag>(entity)) {
            continue;
        }

        if (!registry.all_of<networked_tag>(entity)) {
            continue;
        }

        // Only include entities which are in islands not fully owned by the client
        // since the server allows the client to have full control over entities in
        // the islands where there are no other clients present.
        if (!is_fully_owned_by_client(registry, client_entity, entity)) {
            ctx.pool_snapshot_exporter->export_transient(registry, entity, packet.pools, client_entity);
        }
    }

    if (!packet.pools.empty()) {
        client.packet_signal.publish(client_entity, packet::edyn_packet{packet});
    }
}

static void publish_client_dirty_components(entt::registry &registry,
                                            entt::entity client_entity,
                                            remote_client &client,
                                            aabb_of_interest &aabboi,
                                            double time) {
    // Share dirty entity updates.
    auto packet = packet::general_snapshot{};
    packet.timestamp = time;

    auto &ctx = registry.ctx<server_network_context>();
    auto dirty_view = registry.view<dirty>();
    auto network_dirty_view = registry.view<network_dirty>();

    for (auto entity : aabboi.entities) {
        if (!registry.all_of<networked_tag>(entity)) {
            continue;
        }

        // Add dirty components to snapshot, including for entities
        // owned by the destination client. This does not include components
        // marked as dirty during import of other snapshots since
        // `network_dirty` is used in `server_pool_snapshot_importer` instead.
        if (dirty_view.contains(entity)) {
            auto [dirty] = dirty_view.get(entity);
            ctx.pool_snapshot_exporter->export_dirty_steady(registry, entity, dirty, packet.pools, client_entity);
        }

        // For the components that were marked dirty during a snapshot import,
        // only include updates for those not owned by this client, since that
        // would cause the state that was set by the client to be sent back to
        // the client itself. Transient components are ignored since they're
        // frequently updated via transient snapshots.
        if (network_dirty_view.contains(entity) && !is_fully_owned_by_client(registry, client_entity, entity)) {
            auto [dirty] = network_dirty_view.get(entity);
            ctx.pool_snapshot_exporter->export_dirty_steady(registry, entity, dirty, packet.pools, client_entity);
        }
    }

    if (!packet.pools.empty()) {
        client.packet_signal.publish(client_entity, packet::edyn_packet{packet});
    }
}

static void calculate_client_playout_delay(entt::registry &registry,
                                           entt::entity client_entity,
                                           remote_client &client,
                                           aabb_of_interest &aabboi) {
    auto owner_view = registry.view<entity_owner>();
    auto client_view = registry.view<remote_client>();
    auto biggest_rtt = client.round_trip_time;

    for (auto entity : aabboi.entities) {
        if (!owner_view.contains(entity)) {
            continue;
        }

        auto [owner] = owner_view.get(entity);
        auto [other_client] = client_view.get(owner.client_entity);
        biggest_rtt = std::max(other_client.round_trip_time, biggest_rtt);
    }

    auto &settings = registry.ctx<edyn::settings>();
    auto &server_settings = std::get<server_network_settings>(settings.network_settings);
    auto biggest_latency = biggest_rtt / 2;
    auto playout_delay = biggest_latency * server_settings.playout_delay_multiplier;

    // Update playout delay if the difference is of significance.
    if (std::abs(playout_delay - client.playout_delay) > 0.002) {
        client.playout_delay = playout_delay;

        auto packet = edyn::packet::set_playout_delay{playout_delay};
        client.packet_signal.publish(client_entity, edyn::packet::edyn_packet{packet});
    }
}

static void merge_network_dirty_into_dirty(entt::registry &registry) {
    // Merge components marked as dirty during network import (i.e.
    // `ctx.pool_snapshot_importer->import(...)`) into the regular dirty
    // components so these changes will be pushed into the respective
    // island workers.
    for (auto [entity, network_dirty] : registry.view<edyn::network_dirty>().each()) {
        registry.get_or_emplace<edyn::dirty>(entity).merge(network_dirty);
    }

    // Clear dirty after processing.
    registry.clear<network_dirty>();
}

static void process_aabbs_of_interest(entt::registry &registry, double time) {
    for (auto [client_entity, client, aabboi] : registry.view<remote_client, aabb_of_interest>().each()) {
        process_aabb_of_interest_destroyed_entities(registry, client_entity, client, aabboi, time);
        process_aabb_of_interest_created_entities(registry, client_entity, client, aabboi, time);
        maybe_publish_client_transient_snapshot(registry, client_entity, client, aabboi, time);
        publish_client_dirty_components(registry, client_entity, client, aabboi, time);
        calculate_client_playout_delay(registry, client_entity, client, aabboi);
    }
}

static void server_update_clock_sync(entt::registry &registry, double time) {
    registry.view<remote_client>().each([time] (auto client_entity, remote_client &client) {
        update_clock_sync(client.clock_sync, time, client.round_trip_time, [&] (packet::edyn_packet &&packet) {
            client.packet_signal.publish(client_entity, packet);
        });
    });
}

void update_network_server(entt::registry &registry) {
    auto time = performance_time();
    server_update_clock_sync(registry, time);
    server_process_timed_packets(registry, time);
    update_island_entity_owners(registry);
    update_aabbs_of_interest(registry);
    process_aabbs_of_interest(registry, time);
    publish_pending_created_clients(registry);
    publish_client_current_snapshots(registry);
    merge_network_dirty_into_dirty(registry);
}

template<typename T>
void enqueue_packet(entt::registry &registry, entt::entity client_entity, T &&packet) {
    auto &client = registry.get<remote_client>(client_entity);
    double packet_timestamp;

    if (client.clock_sync.count > 0) {
        packet_timestamp = packet.timestamp + client.clock_sync.time_delta;
    } else {
        packet_timestamp = performance_time() - client.round_trip_time / 2;
    }

    auto insert_it = std::find_if(client.packet_queue.begin(), client.packet_queue.end(),
                                  [packet_timestamp] (auto &&p) { return p.timestamp > packet_timestamp; });
    client.packet_queue.insert(insert_it, timed_packet{packet_timestamp, packet::edyn_packet{std::move(packet)}});
}

void server_receive_packet(entt::registry &registry, entt::entity client_entity, packet::edyn_packet &packet) {
    std::visit([&] (auto &&decoded_packet) {
        using PacketType = std::decay_t<decltype(decoded_packet)>;
        // If it's a timed packet, enqueue for later execution. Process
        // immediately otherwise.
        if constexpr(has_type<PacketType, packet::timed_packets_tuple_t>::value) {
            enqueue_packet(registry, client_entity, decoded_packet);
        } else {
            process_packet(registry, client_entity, decoded_packet);
        }
    }, packet.var);
}

void server_make_client(entt::registry &registry, entt::entity entity) {
    registry.emplace<remote_client>(entity);
    registry.emplace<aabb_of_interest>(entity);

    // `client_created` packets aren't published here at client construction
    // because at this point the caller wouldn't have a chance to receive the
    // packet as a signal in client's packet sink. Thus, this packet is
    // published later on a call to `update_network_server`.
    auto &ctx = registry.ctx<server_network_context>();
    ctx.pending_created_clients.push_back(entity);
}

entt::entity server_make_client(entt::registry &registry) {
    auto entity = registry.create();
    server_make_client(registry, entity);
    return entity;
}

void server_set_client_round_trip_time(entt::registry &registry, entt::entity client_entity, double rtt) {
    auto &client = registry.get<remote_client>(client_entity);
    client.round_trip_time = rtt;
}

void server_notify_created_entities(entt::registry &registry,
                                    entt::entity client_entity,
                                    const std::vector<entt::entity> &entities) {
    auto &ctx = registry.ctx<server_network_context>();
    auto &client = registry.get<edyn::remote_client>(client_entity);

#ifdef EDYN_DEBUG
    // Ensure all entities are networked.
    for (auto entity : entities) {
        EDYN_ASSERT(registry.all_of<networked_tag>(entity));
    }
#endif

    auto packet = edyn::packet::create_entity{};
    packet.timestamp = performance_time();
    packet.entities = entities;

    for (auto entity : packet.entities) {
        ctx.pool_snapshot_exporter->export_all(registry, entity, packet.pools);
    }

    // Sort components to ensure order of construction.
    std::sort(packet.pools.begin(), packet.pools.end(), [] (auto &&lhs, auto &&rhs) {
        return lhs.component_index < rhs.component_index;
    });

    client.packet_signal.publish(client_entity, packet::edyn_packet{packet});
}

}
