#include <algorithm>
#include <cstddef>
#include <cstdlib>

#include "rmw/error_handling.h"
#include "rmw/events_statuses/matched.h"
#include "rmw/events_statuses/liveliness_changed.h"
#include "rmw/events_statuses/liveliness_lost.h"
#include "rmw/events_statuses/offered_deadline_missed.h"
#include "rmw/events_statuses/requested_deadline_missed.h"
#include "rmw/events_statuses/incompatible_qos.h"
#include "rmw/events_statuses/message_lost.h"
#include "rmw/rmw.h"

#include "rmw_zzdds_cpp/endpoint_impl.hpp"
#include "rmw_zzdds_cpp/event_impl.hpp"
#include "rmw_zzdds_cpp/identifier.hpp"

namespace
{
using rmw_zzdds_cpp::PublisherImpl;
using rmw_zzdds_cpp::SubscriptionImpl;

bool publisher_event(rmw_event_type_t type)
{
  return type == RMW_EVENT_PUBLICATION_MATCHED || type == RMW_EVENT_LIVELINESS_LOST ||
         type == RMW_EVENT_OFFERED_DEADLINE_MISSED ||
         type == RMW_EVENT_OFFERED_QOS_INCOMPATIBLE;
}

bool subscription_event(rmw_event_type_t type)
{
  return type == RMW_EVENT_SUBSCRIPTION_MATCHED || type == RMW_EVENT_LIVELINESS_CHANGED ||
         type == RMW_EVENT_REQUESTED_DEADLINE_MISSED ||
         type == RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE || type == RMW_EVENT_MESSAGE_LOST;
}

DDS_StatusMask event_mask(rmw_event_type_t type)
{
  if (type == RMW_EVENT_PUBLICATION_MATCHED) {return DDS_PUBLICATION_MATCHED_STATUS;}
  if (type == RMW_EVENT_SUBSCRIPTION_MATCHED) {return DDS_SUBSCRIPTION_MATCHED_STATUS;}
  if (type == RMW_EVENT_LIVELINESS_CHANGED) {return DDS_LIVELINESS_CHANGED_STATUS;}
  if (type == RMW_EVENT_LIVELINESS_LOST) {return DDS_LIVELINESS_LOST_STATUS;}
  if (type == RMW_EVENT_OFFERED_DEADLINE_MISSED) {return DDS_OFFERED_DEADLINE_MISSED_STATUS;}
  if (type == RMW_EVENT_REQUESTED_DEADLINE_MISSED) {return DDS_REQUESTED_DEADLINE_MISSED_STATUS;}
  if (type == RMW_EVENT_OFFERED_QOS_INCOMPATIBLE) {return DDS_OFFERED_INCOMPATIBLE_QOS_STATUS;}
  if (type == RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE) {return DDS_REQUESTED_INCOMPATIBLE_QOS_STATUS;}
  if (type == RMW_EVENT_MESSAGE_LOST) {return DDS_SAMPLE_LOST_STATUS;}
  return DDS_STATUS_MASK_NONE;
}

rmw_qos_policy_kind_t qos_policy(DDS_QosPolicyId_t id)
{
  switch (id) {
    case DDS_DURABILITY_QOS_POLICY_ID: return RMW_QOS_POLICY_DURABILITY;
    case DDS_DEADLINE_QOS_POLICY_ID: return RMW_QOS_POLICY_DEADLINE;
    case DDS_LIVELINESS_QOS_POLICY_ID: return RMW_QOS_POLICY_LIVELINESS;
    case DDS_RELIABILITY_QOS_POLICY_ID: return RMW_QOS_POLICY_RELIABILITY;
    case DDS_HISTORY_QOS_POLICY_ID: return RMW_QOS_POLICY_HISTORY;
    case DDS_LIFESPAN_QOS_POLICY_ID: return RMW_QOS_POLICY_LIFESPAN;
    default: return RMW_QOS_POLICY_INVALID;
  }
}

// Each rmw event keeps its endpoint's zzdds status in PublisherImpl/
// SubscriptionImpl (publication_matched, ...), its *_change fields holding the
// changes not yet taken. read_status reads zzdds's current status and adds what
// changed since the stored one, computed from the counts rather than taken
// from zzdds's own *_change fields. The listener, rmw_take_event and event
// initialization all go through it, under event_mutex, so each read sees state
// at least as new as the one before and every change is counted exactly once.
// Reading zzdds under event_mutex cannot deadlock against a listener: zzdds
// calls no listener while holding its own locks.
//
// event_count is how many untaken events have been counted (triggering the
// event), event_reported how many of those a callback has been told about, so
// registering a callback reports only the rest. A take resets both. The
// listener for every initialized event stays installed whether or not a
// callback is registered, and counts the events it finds. zzdds calls it after
// releasing its lock, so a take can read a change before the change's listener
// runs; the listener then finds nothing left to count.
//
// For every status but liveliness changed, the number of changes since the
// last take is exact: untaken_events computes it from the stored status, and
// the listener counts what event_count does not include yet. Liveliness
// changed has no such count (writers going alive and not alive cancel out in
// its counts), so its listener counts one event when the status zzdds passed
// differs from the one at the last take. That still notifies when changes
// cancel out, unless their listeners all run after the last of them; a
// listener that runs after a take which also read a later change counts one
// event too many.

void add_changes(
  DDS_PublicationMatchedStatus & stored, const DDS_PublicationMatchedStatus & now)
{
  const int32_t total = now.total_count - stored.total_count;
  const int32_t current = now.current_count - stored.current_count;
  stored.total_count_change += total;
  stored.current_count_change += current;
  stored.total_count = now.total_count;
  stored.current_count = now.current_count;
  stored.last_subscription_handle = now.last_subscription_handle;
}

void add_changes(
  DDS_SubscriptionMatchedStatus & stored, const DDS_SubscriptionMatchedStatus & now)
{
  const int32_t total = now.total_count - stored.total_count;
  const int32_t current = now.current_count - stored.current_count;
  stored.total_count_change += total;
  stored.current_count_change += current;
  stored.total_count = now.total_count;
  stored.current_count = now.current_count;
  stored.last_publication_handle = now.last_publication_handle;
}

void add_changes(DDS_LivelinessChangedStatus & stored, const DDS_LivelinessChangedStatus & now)
{
  const int32_t alive = now.alive_count - stored.alive_count;
  const int32_t not_alive = now.not_alive_count - stored.not_alive_count;
  stored.alive_count_change += alive;
  stored.not_alive_count_change += not_alive;
  stored.alive_count = now.alive_count;
  stored.not_alive_count = now.not_alive_count;
  stored.last_publication_handle = now.last_publication_handle;
}

// Statuses that only count occurrences, plus the last-occurrence field some
// carry (copied when the count moves).
template<typename Status>
bool add_count_changes(Status & stored, const Status & now)
{
  const int32_t total = now.total_count - stored.total_count;
  stored.total_count_change += total;
  stored.total_count = now.total_count;
  return total != 0;
}
void add_changes(DDS_LivelinessLostStatus & stored, const DDS_LivelinessLostStatus & now)
{
  (void)add_count_changes(stored, now);
}
void add_changes(DDS_SampleLostStatus & stored, const DDS_SampleLostStatus & now)
{
  (void)add_count_changes(stored, now);
}
template<typename Status>
void add_deadline_changes(Status & stored, const Status & now)
{
  if (add_count_changes(stored, now)) {stored.last_instance_handle = now.last_instance_handle;}
}
void add_changes(
  DDS_OfferedDeadlineMissedStatus & stored, const DDS_OfferedDeadlineMissedStatus & now)
{
  add_deadline_changes(stored, now);
}
void add_changes(
  DDS_RequestedDeadlineMissedStatus & stored, const DDS_RequestedDeadlineMissedStatus & now)
{
  add_deadline_changes(stored, now);
}
// The policies sequence is not kept: rmw reports only the last policy.
template<typename Status>
void add_incompatible_qos_changes(Status & stored, const Status & now)
{
  if (add_count_changes(stored, now)) {stored.last_policy_id = now.last_policy_id;}
}
void add_changes(
  DDS_OfferedIncompatibleQosStatus & stored, const DDS_OfferedIncompatibleQosStatus & now)
{
  add_incompatible_qos_changes(stored, now);
}
void add_changes(
  DDS_RequestedIncompatibleQosStatus & stored, const DDS_RequestedIncompatibleQosStatus & now)
{
  add_incompatible_qos_changes(stored, now);
}

// Number of events behind a stored status's untaken changes. Each match raises
// total and current by one and each unmatch lowers current only. Liveliness
// changes are visible only as net changes, so for liveliness changed this is
// an estimate, counting a transition once; it is used only for changes from
// before the event was initialized (see the comment above add_changes).
template<typename Status>
size_t untaken_matched_events(const Status & s)
{
  return static_cast<size_t>(s.total_count_change) +
         static_cast<size_t>(s.total_count_change - s.current_count_change);
}
size_t untaken_events(const DDS_PublicationMatchedStatus & s) {return untaken_matched_events(s);}
size_t untaken_events(const DDS_SubscriptionMatchedStatus & s) {return untaken_matched_events(s);}
size_t untaken_events(const DDS_LivelinessChangedStatus & s)
{
  return static_cast<size_t>(
    std::max(std::abs(s.alive_count_change), std::abs(s.not_alive_count_change)));
}
template<typename Status>
size_t untaken_events(const Status & s) {return static_cast<size_t>(s.total_count_change);}

template<typename Status, typename Entity>
bool read_into(
  Status & stored, Entity entity, DDS_ReturnCode_t (* get)(Entity, Status *))
{
  Status now{};
  if (get(entity, &now) != DDS_RETCODE_OK) {return false;}
  add_changes(stored, now);
  return true;
}

template<typename Entity>
bool read_into(
  DDS_OfferedIncompatibleQosStatus & stored, Entity entity,
  DDS_ReturnCode_t (* get)(Entity, DDS_OfferedIncompatibleQosStatus *))
{
  DDS_OfferedIncompatibleQosStatus now{};
  if (get(entity, &now) != DDS_RETCODE_OK) {return false;}
  add_changes(stored, now);
  DDS_QosPolicyCountSeq_free(&now.policies);
  return true;
}

template<typename Entity>
bool read_into(
  DDS_RequestedIncompatibleQosStatus & stored, Entity entity,
  DDS_ReturnCode_t (* get)(Entity, DDS_RequestedIncompatibleQosStatus *))
{
  DDS_RequestedIncompatibleQosStatus now{};
  if (get(entity, &now) != DDS_RETCODE_OK) {return false;}
  add_changes(stored, now);
  DDS_QosPolicyCountSeq_free(&now.policies);
  return true;
}

// Calls `visit(stored_status)` for `type`'s stored status on impl.
template<typename Visit>
auto visit_status(PublisherImpl * impl, rmw_event_type_t type, Visit && visit)
{
  switch (type) {
    case RMW_EVENT_PUBLICATION_MATCHED: return visit(impl->publication_matched);
    case RMW_EVENT_LIVELINESS_LOST: return visit(impl->liveliness_lost);
    case RMW_EVENT_OFFERED_DEADLINE_MISSED: return visit(impl->offered_deadline_missed);
    default: return visit(impl->offered_incompatible_qos);
  }
}

template<typename Visit>
auto visit_status(SubscriptionImpl * impl, rmw_event_type_t type, Visit && visit)
{
  switch (type) {
    case RMW_EVENT_SUBSCRIPTION_MATCHED: return visit(impl->subscription_matched);
    case RMW_EVENT_LIVELINESS_CHANGED: return visit(impl->liveliness_changed);
    case RMW_EVENT_REQUESTED_DEADLINE_MISSED: return visit(impl->requested_deadline_missed);
    case RMW_EVENT_MESSAGE_LOST: return visit(impl->sample_lost);
    default: return visit(impl->requested_incompatible_qos);
  }
}

// See the comment above add_changes. Caller holds impl->event_mutex.
bool read_status(PublisherImpl * impl, rmw_event_type_t type)
{
  switch (type) {
    case RMW_EVENT_PUBLICATION_MATCHED:
      return read_into(
        impl->publication_matched, impl->writer, DDS_DataWriter_get_publication_matched_status);
    case RMW_EVENT_LIVELINESS_LOST:
      return read_into(
        impl->liveliness_lost, impl->writer, DDS_DataWriter_get_liveliness_lost_status);
    case RMW_EVENT_OFFERED_DEADLINE_MISSED:
      return read_into(
        impl->offered_deadline_missed, impl->writer,
        DDS_DataWriter_get_offered_deadline_missed_status);
    default:
      return read_into(
        impl->offered_incompatible_qos, impl->writer,
        DDS_DataWriter_get_offered_incompatible_qos_status);
  }
}

bool read_status(SubscriptionImpl * impl, rmw_event_type_t type)
{
  switch (type) {
    case RMW_EVENT_SUBSCRIPTION_MATCHED:
      return read_into(
        impl->subscription_matched, impl->reader, DDS_DataReader_get_subscription_matched_status);
    case RMW_EVENT_LIVELINESS_CHANGED:
      return read_into(
        impl->liveliness_changed, impl->reader, DDS_DataReader_get_liveliness_changed_status);
    case RMW_EVENT_REQUESTED_DEADLINE_MISSED:
      return read_into(
        impl->requested_deadline_missed, impl->reader,
        DDS_DataReader_get_requested_deadline_missed_status);
    case RMW_EVENT_MESSAGE_LOST:
      return read_into(impl->sample_lost, impl->reader, DDS_DataReader_get_sample_lost_status);
    default:
      return read_into(
        impl->requested_incompatible_qos, impl->reader,
        DDS_DataReader_get_requested_incompatible_qos_status);
  }
}

// A zzdds listener for `type` fired: updates the stored status and counts the
// events `uncounted` finds (called under event_mutex after the update),
// triggering the event and telling its callback if there are any. See the
// comment above add_changes.
template<typename Impl, typename Uncounted>
void on_status(Impl * impl, rmw_event_type_t type, Uncounted && uncounted)
{
  rmw_event_callback_t callback = nullptr;
  const void * user_data = nullptr;
  size_t added = 0U;
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    (void)read_status(impl, type);
    added = uncounted();
    if (added == 0U) {return;}
    impl->event_count[type] += added;
    (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[type], true);
    callback = impl->event_callbacks[type];
    user_data = impl->event_user_data[type];
    if (callback != nullptr) {impl->event_reported[type] += added;}
  }
  if (callback != nullptr) {callback(user_data, added);}
}

// For every status but liveliness changed: the untaken events not counted yet.
// The status zzdds passes is not used: listeners can run concurrently, so it
// may be older than the stored one.
template<typename Impl>
void on_counted_status(Impl * impl, rmw_event_type_t type)
{
  on_status(
    impl, type, [impl, type] {
      const size_t untaken =
      visit_status(impl, type, [](const auto & s) {return untaken_events(s);});
      return untaken > impl->event_count[type] ? untaken - impl->event_count[type] : 0U;
    });
}

void publication_matched_listener(DDS_DataWriter, const DDS_PublicationMatchedStatus *, void * data)
{
  on_counted_status(static_cast<PublisherImpl *>(data), RMW_EVENT_PUBLICATION_MATCHED);
}
void liveliness_lost_listener(DDS_DataWriter, const DDS_LivelinessLostStatus *, void * data)
{
  on_counted_status(static_cast<PublisherImpl *>(data), RMW_EVENT_LIVELINESS_LOST);
}
void offered_deadline_listener(
  DDS_DataWriter, const DDS_OfferedDeadlineMissedStatus *, void * data)
{
  on_counted_status(static_cast<PublisherImpl *>(data), RMW_EVENT_OFFERED_DEADLINE_MISSED);
}
void offered_incompatible_qos_listener(
  DDS_DataWriter, const DDS_OfferedIncompatibleQosStatus *, void * data)
{
  on_counted_status(static_cast<PublisherImpl *>(data), RMW_EVENT_OFFERED_QOS_INCOMPATIBLE);
}
void subscription_matched_listener(
  DDS_DataReader, const DDS_SubscriptionMatchedStatus *, void * data)
{
  on_counted_status(static_cast<SubscriptionImpl *>(data), RMW_EVENT_SUBSCRIPTION_MATCHED);
}
// One event if the status zzdds passed differs from the one at the last take,
// which the stored status still holds as its counts minus its untaken changes.
void liveliness_changed_listener(
  DDS_DataReader, const DDS_LivelinessChangedStatus * status, void * data)
{
  auto * impl = static_cast<SubscriptionImpl *>(data);
  on_status(
    impl, RMW_EVENT_LIVELINESS_CHANGED, [impl, status] {
      const DDS_LivelinessChangedStatus & stored = impl->liveliness_changed;
      const bool changed =
      status->alive_count != stored.alive_count - stored.alive_count_change ||
      status->not_alive_count != stored.not_alive_count - stored.not_alive_count_change;
      return changed ? 1U : 0U;
    });
}
void requested_deadline_listener(
  DDS_DataReader, const DDS_RequestedDeadlineMissedStatus *, void * data)
{
  on_counted_status(static_cast<SubscriptionImpl *>(data), RMW_EVENT_REQUESTED_DEADLINE_MISSED);
}
void sample_lost_listener(DDS_DataReader, const DDS_SampleLostStatus *, void * data)
{
  on_counted_status(static_cast<SubscriptionImpl *>(data), RMW_EVENT_MESSAGE_LOST);
}
void requested_incompatible_qos_listener(
  DDS_DataReader, const DDS_RequestedIncompatibleQosStatus *, void * data)
{
  on_counted_status(static_cast<SubscriptionImpl *>(data), RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE);
}

// A callback was registered or cleared for `type`: tell a new callback about
// untaken events no callback has been told about yet.
template<typename Impl>
void after_set_callback(Impl * impl, rmw_event_type_t type)
{
  rmw_event_callback_t callback = nullptr;
  const void * user_data = nullptr;
  size_t unreported = 0U;
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    callback = impl->event_callbacks[type];
    user_data = impl->event_user_data[type];
    if (callback != nullptr) {
      unreported = impl->event_count[type] - impl->event_reported[type];
      impl->event_reported[type] = impl->event_count[type];
    }
  }
  if (unreported != 0U) {callback(user_data, unreported);}
}

// Not gated by any rmw_event_type_t/DDS_StatusMask -- zzdds dispatches this
// zzdds::DataReaderListenerEx extension callback unconditionally (see
// apply_subscription_listener's caller-facing doc comment). Always installed,
// regardless of whether the application has registered any rmw event
// callback for this subscription.
void reliable_writer_ready_listener(DDS_InstanceHandle_t, bool is_ready, void * data)
{
  auto * impl = static_cast<SubscriptionImpl *>(data);
  impl->reliable_writer_ready_count.fetch_add(
    is_ready ? 1 : -1, std::memory_order_relaxed);
}

// Installs impl's writer listener for every initialized event's status (see
// the comment above add_changes).
bool apply_publisher_listener(PublisherImpl * impl)
{
  DDS_DataWriterListener listener{};
  listener.listener_data = impl;
  listener.on_publication_matched = publication_matched_listener;
  listener.on_liveliness_lost = liveliness_lost_listener;
  listener.on_offered_deadline_missed = offered_deadline_listener;
  listener.on_offered_incompatible_qos = offered_incompatible_qos_listener;
  DDS_StatusMask mask = DDS_STATUS_MASK_NONE;
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    for (rmw_event_type_t type : {
        RMW_EVENT_PUBLICATION_MATCHED, RMW_EVENT_LIVELINESS_LOST,
        RMW_EVENT_OFFERED_DEADLINE_MISSED, RMW_EVENT_OFFERED_QOS_INCOMPATIBLE})
    {
      if (impl->event_guards[type] != nullptr) {mask |= event_mask(type);}
    }
  }
  return DDS_DataWriter_set_listener(
    impl->writer, mask == DDS_STATUS_MASK_NONE ? nullptr : &listener, mask) == DDS_RETCODE_OK;
}

bool apply_listener(PublisherImpl * impl) {return apply_publisher_listener(impl);}
bool apply_listener(SubscriptionImpl * impl)
{
  return rmw_zzdds_cpp::apply_subscription_listener(impl);
}

// Creates `type`'s guard condition, installs the listener that counts its
// events, and counts changes from before the event existed (see
// untaken_events) if nothing has been counted yet.
template<typename Impl>
rmw_ret_t initialize_event(rmw_event_t * event, Impl * impl, rmw_event_type_t type)
{
  if (event == nullptr || event->implementation_identifier != nullptr ||
    event->data != nullptr || event->event_type != RMW_EVENT_INVALID)
  {
    RMW_SET_ERROR_MSG("a zero-initialized event and valid endpoint are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (event_mask(type) == DDS_STATUS_MASK_NONE) {
    RMW_SET_ERROR_MSG("event type is not supported by rmw_zzdds_cpp");
    return RMW_RET_UNSUPPORTED;
  }
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    if (impl->event_guards[type] == nullptr) {
      impl->event_guards[type] = zzdds_create_guardcondition();
    }
    if (impl->event_guards[type] == nullptr) {
      RMW_SET_ERROR_MSG("failed to create the event guard condition");
      return RMW_RET_BAD_ALLOC;
    }
  }
  if (!apply_listener(impl)) {
    RMW_SET_ERROR_MSG("failed to install the zzdds event listener");
    return RMW_RET_ERROR;
  }
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    (void)read_status(impl, type);
    if (impl->event_count[type] == 0U) {
      impl->event_count[type] =
        visit_status(impl, type, [](const auto & s) {return untaken_events(s);});
    }
    (void)DDS_GuardCondition_set_trigger_value(
      impl->event_guards[type], impl->event_count[type] != 0U);
  }
  event->implementation_identifier = rmw_zzdds_cpp::identifier;
  event->data = impl;
  event->event_type = type;
  return RMW_RET_OK;
}
}  // namespace

namespace rmw_zzdds_cpp
{
// The event's guard condition: triggered by its listener, which consumes
// zzdds's own status change (see the comment above add_changes).
DDS_Condition event_condition(const rmw_event_t * event)
{
  if (publisher_event(event->event_type)) {
    auto * impl = static_cast<PublisherImpl *>(event->data);
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    return DDS_GuardCondition_as_DDS_Condition(impl->event_guards[event->event_type]);
  }
  auto * impl = static_cast<SubscriptionImpl *>(event->data);
  const std::lock_guard<std::mutex> lock(impl->event_mutex);
  return DDS_GuardCondition_as_DDS_Condition(impl->event_guards[event->event_type]);
}

ContextImpl * event_context(const rmw_event_t * event)
{
  if (publisher_event(event->event_type)) {
    return static_cast<PublisherImpl *>(event->data)->context;
  }
  return static_cast<SubscriptionImpl *>(event->data)->context;
}

template<typename Impl>
void cleanup(Impl * impl)
{
  for (DDS_GuardCondition guard : impl->event_guards) {
    if (guard != nullptr) {zzdds_destroy_guardcondition(guard);}
  }
}

void cleanup_endpoint_events(PublisherImpl * impl) {cleanup(impl);}
void cleanup_endpoint_events(SubscriptionImpl * impl) {cleanup(impl);}

namespace
{
// Fills `listener` with impl's full subscription listener and returns the
// status mask for the rmw event callbacks currently registered on impl.
DDS_StatusMask subscription_listener(SubscriptionImpl * impl, zzdds_DataReaderListenerEx & listener)
{
  DDS_StatusMask mask = DDS_STATUS_MASK_NONE;
  {
    // Every initialized event's status (see the comment above add_changes).
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    for (rmw_event_type_t type : {
        RMW_EVENT_SUBSCRIPTION_MATCHED, RMW_EVENT_LIVELINESS_CHANGED,
        RMW_EVENT_REQUESTED_DEADLINE_MISSED, RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE,
        RMW_EVENT_MESSAGE_LOST})
    {
      if (impl->event_guards[type] != nullptr) {mask |= event_mask(type);}
    }
  }
  listener = zzdds_DataReaderListenerEx{};
  listener.listener_data = impl;
  listener.on_subscription_matched = subscription_matched_listener;
  listener.on_liveliness_changed = liveliness_changed_listener;
  listener.on_requested_deadline_missed = requested_deadline_listener;
  listener.on_requested_incompatible_qos = requested_incompatible_qos_listener;
  listener.on_sample_lost = sample_lost_listener;
  listener.on_reliable_writer_ready = reliable_writer_ready_listener;
  return mask;
}
}  // namespace

DDS_DataReader create_subscription_reader(
  SubscriptionImpl * impl, DDS_TopicDescription topic_description, const DDS_DataReaderQos * qos)
{
  zzdds_DataReaderListenerEx listener;
  const DDS_StatusMask mask = subscription_listener(impl, listener);
  return zzdds_Subscriber_create_datareader_ex(
    DDS_Subscriber_as_zzdds_Subscriber(impl->context->dds.subscriber()), topic_description, qos,
    &listener, mask);
}

bool apply_subscription_listener(SubscriptionImpl * impl)
{
  zzdds_DataReaderListenerEx listener;
  const DDS_StatusMask mask = subscription_listener(impl, listener);
  return zzdds_DataReader_set_listener_ex(
    DDS_DataReader_as_zzdds_DataReader(impl->reader), &listener, mask) == DDS_RETCODE_OK;
}
}  // namespace rmw_zzdds_cpp

extern "C"
{
rmw_ret_t rmw_publisher_event_init(
  rmw_event_t * event, const rmw_publisher_t * publisher, rmw_event_type_t type)
{
  if (publisher == nullptr || publisher->data == nullptr) {return RMW_RET_INVALID_ARGUMENT;}
  if (publisher->implementation_identifier != rmw_zzdds_cpp::identifier) {
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (!publisher_event(type)) {return RMW_RET_UNSUPPORTED;}
  return initialize_event(event, static_cast<PublisherImpl *>(publisher->data), type);
}

rmw_ret_t rmw_subscription_event_init(
  rmw_event_t * event, const rmw_subscription_t * subscription, rmw_event_type_t type)
{
  if (subscription == nullptr || subscription->data == nullptr) {return RMW_RET_INVALID_ARGUMENT;}
  if (subscription->implementation_identifier != rmw_zzdds_cpp::identifier) {
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (!subscription_event(type)) {return RMW_RET_UNSUPPORTED;}
  return initialize_event(event, static_cast<SubscriptionImpl *>(subscription->data), type);
}

rmw_ret_t rmw_take_event(const rmw_event_t * event, void * event_info, bool * taken)
{
  if (event == nullptr || event_info == nullptr || taken == nullptr || event->data == nullptr) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (event->implementation_identifier != rmw_zzdds_cpp::identifier) {
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  *taken = false;
  if (publisher_event(event->event_type)) {
    auto * impl = static_cast<PublisherImpl *>(event->data);
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    if (!read_status(impl, event->event_type)) {return RMW_RET_ERROR;}
    switch (event->event_type) {
      case RMW_EVENT_PUBLICATION_MATCHED: {
          auto & status = impl->publication_matched;
          auto * output = static_cast<rmw_matched_status_t *>(event_info);
          output->total_count = static_cast<size_t>(status.total_count);
          output->total_count_change = static_cast<size_t>(status.total_count_change);
          output->current_count = static_cast<size_t>(status.current_count);
          output->current_count_change = status.current_count_change;
          status.total_count_change = 0;
          status.current_count_change = 0;
          break;
        }
      case RMW_EVENT_LIVELINESS_LOST: {
          auto & status = impl->liveliness_lost;
          auto * output = static_cast<rmw_liveliness_lost_status_t *>(event_info);
          output->total_count = status.total_count;
          output->total_count_change = status.total_count_change;
          status.total_count_change = 0;
          break;
        }
      case RMW_EVENT_OFFERED_DEADLINE_MISSED: {
          auto & status = impl->offered_deadline_missed;
          auto * output = static_cast<rmw_offered_deadline_missed_status_t *>(event_info);
          output->total_count = status.total_count;
          output->total_count_change = status.total_count_change;
          status.total_count_change = 0;
          break;
        }
      default: {
          auto & status = impl->offered_incompatible_qos;
          auto * output = static_cast<rmw_offered_qos_incompatible_event_status_t *>(event_info);
          output->total_count = status.total_count;
          output->total_count_change = status.total_count_change;
          output->last_policy_kind = qos_policy(status.last_policy_id);
          status.total_count_change = 0;
          break;
        }
    }
    impl->event_count[event->event_type] = 0U;
    impl->event_reported[event->event_type] = 0U;
    (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event->event_type], false);
  } else if (subscription_event(event->event_type)) {
    auto * impl = static_cast<SubscriptionImpl *>(event->data);
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    if (!read_status(impl, event->event_type)) {return RMW_RET_ERROR;}
    switch (event->event_type) {
      case RMW_EVENT_SUBSCRIPTION_MATCHED: {
          auto & status = impl->subscription_matched;
          auto * output = static_cast<rmw_matched_status_t *>(event_info);
          output->total_count = static_cast<size_t>(status.total_count);
          output->total_count_change = static_cast<size_t>(status.total_count_change);
          output->current_count = static_cast<size_t>(status.current_count);
          output->current_count_change = status.current_count_change;
          status.total_count_change = 0;
          status.current_count_change = 0;
          break;
        }
      case RMW_EVENT_LIVELINESS_CHANGED: {
          auto & status = impl->liveliness_changed;
          auto * output = static_cast<rmw_liveliness_changed_status_t *>(event_info);
          output->alive_count = status.alive_count;
          output->not_alive_count = status.not_alive_count;
          output->alive_count_change = status.alive_count_change;
          output->not_alive_count_change = status.not_alive_count_change;
          status.alive_count_change = 0;
          status.not_alive_count_change = 0;
          break;
        }
      case RMW_EVENT_REQUESTED_DEADLINE_MISSED: {
          auto & status = impl->requested_deadline_missed;
          auto * output = static_cast<rmw_requested_deadline_missed_status_t *>(event_info);
          output->total_count = status.total_count;
          output->total_count_change = status.total_count_change;
          status.total_count_change = 0;
          break;
        }
      case RMW_EVENT_MESSAGE_LOST: {
          auto & status = impl->sample_lost;
          auto * output = static_cast<rmw_message_lost_status_t *>(event_info);
          output->total_count = static_cast<size_t>(status.total_count);
          output->total_count_change = static_cast<size_t>(status.total_count_change);
          status.total_count_change = 0;
          break;
        }
      default: {
          auto & status = impl->requested_incompatible_qos;
          auto * output = static_cast<rmw_requested_qos_incompatible_event_status_t *>(event_info);
          output->total_count = status.total_count;
          output->total_count_change = status.total_count_change;
          output->last_policy_kind = qos_policy(status.last_policy_id);
          status.total_count_change = 0;
          break;
        }
    }
    impl->event_count[event->event_type] = 0U;
    impl->event_reported[event->event_type] = 0U;
    (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event->event_type], false);
  } else {
    return RMW_RET_UNSUPPORTED;
  }
  *taken = true;
  return RMW_RET_OK;
}

bool rmw_event_type_is_supported(rmw_event_type_t type)
{
  return publisher_event(type) || subscription_event(type);
}

rmw_ret_t rmw_event_set_callback(
  rmw_event_t * event, rmw_event_callback_t callback, const void * user_data)
{
  if (event == nullptr || event->data == nullptr) {return RMW_RET_INVALID_ARGUMENT;}
  if (event->implementation_identifier != rmw_zzdds_cpp::identifier) {
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (publisher_event(event->event_type)) {
    auto * impl = static_cast<PublisherImpl *>(event->data);
    {
      const std::lock_guard<std::mutex> lock(impl->event_mutex);
      impl->event_callbacks[event->event_type] = callback;
      impl->event_user_data[event->event_type] = user_data;
    }
    after_set_callback(impl, event->event_type);
    return RMW_RET_OK;
  }
  auto * impl = static_cast<SubscriptionImpl *>(event->data);
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    impl->event_callbacks[event->event_type] = callback;
    impl->event_user_data[event->event_type] = user_data;
  }
  // The listener counting this event's changes is already installed (see
  // initialize_event).
  after_set_callback(impl, event->event_type);
  return RMW_RET_OK;
}

}
