#ifndef RMW_ZZDDS_CPP__SERVICE_IMPL_HPP_
#define RMW_ZZDDS_CPP__SERVICE_IMPL_HPP_

#include <atomic>

#include "rmw/types.h"
#include "rmw_zzdds_cpp/context_impl.hpp"
#include "rmw_zzdds_cpp/type_support.hpp"

namespace rmw_zzdds_cpp
{
struct ClientImpl final
{
  ClientImpl(ContextImpl * c, const rmw_node_t * n, rmw_publisher_t * p,
    rmw_subscription_t * s, ServiceTypeSupport t)
  : context(c), node(n), request_publisher(p), response_subscription(s), typesupport(t) {}
  ContextImpl * context;
  const rmw_node_t * node;
  rmw_publisher_t * request_publisher;
  rmw_subscription_t * response_subscription;
  ServiceTypeSupport typesupport;
  std::atomic_int64_t next_sequence{1};
};

struct ServiceImpl final
{
  ServiceImpl(ContextImpl * c, const rmw_node_t * n, rmw_subscription_t * s,
    rmw_publisher_t * p, ServiceTypeSupport t)
  : context(c), node(n), request_subscription(s), response_publisher(p), typesupport(t) {}
  ContextImpl * context;
  const rmw_node_t * node;
  rmw_subscription_t * request_subscription;
  rmw_publisher_t * response_publisher;
  ServiceTypeSupport typesupport;
};
}  // namespace rmw_zzdds_cpp

#endif
