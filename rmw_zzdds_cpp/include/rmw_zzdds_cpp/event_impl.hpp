#ifndef RMW_ZZDDS_CPP__EVENT_IMPL_HPP_
#define RMW_ZZDDS_CPP__EVENT_IMPL_HPP_

#include "rmw/event.h"
#include "zzdds_c.h"
#include "rmw_zzdds_cpp/context_impl.hpp"

namespace rmw_zzdds_cpp
{
struct PublisherImpl;
struct SubscriptionImpl;
DDS_Condition event_condition(const rmw_event_t * event);
ContextImpl * event_context(const rmw_event_t * event);
void cleanup_endpoint_events(PublisherImpl * impl);
void cleanup_endpoint_events(SubscriptionImpl * impl);
}  // namespace rmw_zzdds_cpp

#endif
