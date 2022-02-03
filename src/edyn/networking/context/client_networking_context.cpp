#include "edyn/networking/context/client_networking_context.hpp"
#include "edyn/networking/comp/networked_comp.hpp"
#include "edyn/networking/comp/transient_comp.hpp"
#include "edyn/networking/util/client_pool_snapshot_importer.hpp"
#include "edyn/networking/extrapolation_job.hpp"

namespace edyn {

client_networking_context::client_networking_context()
    : pool_snapshot_importer(new client_pool_snapshot_importer_impl(networked_components, {}))
    , pool_snapshot_exporter(new client_pool_snapshot_exporter_impl(networked_components, transient_components, {}))
    , extrapolation_component_pool_import_func(internal::make_extrapolation_component_pools_import_func(networked_components))
    , extrapolation_component_pool_import_by_id_func(internal::make_extrapolation_component_pools_import_by_id_func(networked_components))
{}

}