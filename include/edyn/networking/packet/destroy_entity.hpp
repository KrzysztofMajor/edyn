#ifndef EDYN_NETWORKING_PACKET_DESTROY_ENTITY_HPP
#define EDYN_NETWORKING_PACKET_DESTROY_ENTITY_HPP

#include <vector>
#include <entt/entity/fwd.hpp>

namespace edyn::packet {

struct destroy_entity {
    std::vector<entt::entity> entities;
};

template<typename Archive>
void serialize(Archive &archive, destroy_entity &packet) {
    archive(packet.entities);
}

}

#endif // EDYN_NETWORKING_PACKET_DESTROY_ENTITY_HPP
