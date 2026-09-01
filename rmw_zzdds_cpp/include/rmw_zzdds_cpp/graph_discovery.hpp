#ifndef RMW_ZZDDS_CPP__GRAPH_DISCOVERY_HPP_
#define RMW_ZZDDS_CPP__GRAPH_DISCOVERY_HPP_

#include "rmw/ret_types.h"
#include "rmw/init.h"

namespace rmw_zzdds_cpp
{
struct ContextImpl;

rmw_ret_t refresh_discovered_endpoints(ContextImpl * context);

rmw_ret_t initialize_graph_channel(rmw_context_t * context);
void stop_graph_listener(ContextImpl * context) noexcept;
rmw_ret_t finalize_graph_channel(rmw_context_t * context) noexcept;
}  // namespace rmw_zzdds_cpp

#endif  // RMW_ZZDDS_CPP__GRAPH_DISCOVERY_HPP_
