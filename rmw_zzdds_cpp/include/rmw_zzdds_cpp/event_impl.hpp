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
// (Re)installs impl->reader's full DDS_DataReader listener: the event-driven
// base callbacks rmw_event_set_callback's registered rmw_event_type_ts need,
// plus an always-on zzdds DataReaderListenerEx::on_reliable_writer_ready
// callback that maintains impl->reliable_writer_ready_count -- unconditional
// on the DDS_StatusMask, since zzdds dispatches that extension callback
// unmasked. Call once at subscription creation (to start readiness tracking
// immediately) and again any time impl->event_callbacks changes -- the base
// DDS_DataReaderListener path a plain DDS_DataReader_set_listener call would
// use replaces the whole listener, including this extension field. Returns
// false on a genuine zzdds C-ABI failure (mirrors the underlying
// zzdds_DataReader_set_listener_ex return code).
bool apply_subscription_listener(SubscriptionImpl * impl);
}  // namespace rmw_zzdds_cpp

#endif
