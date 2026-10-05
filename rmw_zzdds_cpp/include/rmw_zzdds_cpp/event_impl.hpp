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
// A subscription's full DDS_DataReader listener is the event-driven base
// callbacks rmw_event_set_callback's registered rmw_event_type_ts need, plus
// an always-on zzdds DataReaderListenerEx::on_reliable_writer_ready callback
// that maintains impl->reliable_writer_ready_count -- unconditional on the
// DDS_StatusMask, since zzdds dispatches that extension callback unmasked.
//
// Creates impl's DDS_DataReader (on impl->context's subscriber) with that
// listener installed from the start. A writer discovered earlier matches the
// reader inside creation, so a listener attached afterwards could miss its
// on_reliable_writer_ready (and impl->reliable_writer_ready_count would never
// count it). impl->reader is not read; the caller stores the result there.
// Returns nullptr on failure.
DDS_DataReader create_subscription_reader(
  SubscriptionImpl * impl, DDS_TopicDescription topic_description, const DDS_DataReaderQos * qos);
// Folds `reader`'s SubscriptionMatchedStatus into impl->subscription_matched
// (see SubscriptionMatchedContinuity::fold), passing the reader's matched
// publications when an inherited one is still outstanding. `reader` is the
// reader the status came from, which during a content-filter change is not
// yet impl->reader. Caller holds impl->event_mutex.
void accumulate_subscription_matched(
  SubscriptionImpl * impl, DDS_DataReader reader,
  const DDS_SubscriptionMatchedStatus & reader_status);
// Reinstalls impl->reader's full listener. Call any time impl->event_callbacks
// changes -- the base DDS_DataReaderListener path a plain
// DDS_DataReader_set_listener call would use replaces the whole listener,
// including the extension field. Returns false on a genuine zzdds C-ABI
// failure (mirrors the underlying zzdds_DataReader_set_listener_ex return
// code).
bool apply_subscription_listener(SubscriptionImpl * impl);
}  // namespace rmw_zzdds_cpp

#endif
