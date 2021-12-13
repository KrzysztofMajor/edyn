#ifndef EDYN_COMP_CONTINUOUS_HPP
#define EDYN_COMP_CONTINUOUS_HPP

#include "edyn/config/config.h"
#include <array>
#include <entt/core/type_info.hpp>

namespace edyn {

/**
 * @brief Specifies a set of component types that the island worker must send
 * back to the coordinator after every step of the simulation.
 * @remark The types are referred to the index of the component in the current
 * `component_source_index` as to make them stable among different machines to
 * allow this component to be shared between client and server in a networked
 * simulation.
 */
struct continuous {
    static constexpr size_t max_size = 16;
    std::array<size_t, max_size> indices;
    size_t size {0};

    void insert(size_t index) {
        indices[size++] = index;
        EDYN_ASSERT(size <= max_size);
    }

    void remove(size_t index) {
        for (size_t i = 0; i < size; ++i) {
            if (indices[i] == index) {
                indices[i] = indices[--size];
                break;
            }
        }
    }
};

}

#endif // EDYN_COMP_CONTINUOUS_HPP
