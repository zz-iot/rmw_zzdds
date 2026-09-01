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

void publication_matched_listener(
  DDS_DataWriter, const DDS_PublicationMatchedStatus * status, void * data)
{
  auto * impl = static_cast<PublisherImpl *>(data);
  rmw_event_callback_t callback = nullptr;
  const void * user_data = nullptr;
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    impl->publication_matched.total_count = status->total_count;
    impl->publication_matched.total_count_change += status->total_count_change;
    impl->publication_matched.current_count = status->current_count;
    impl->publication_matched.current_count_change += status->current_count_change;
    impl->publication_matched_pending = true;
    callback = impl->event_callbacks[RMW_EVENT_PUBLICATION_MATCHED];
    user_data = impl->event_user_data[RMW_EVENT_PUBLICATION_MATCHED];
    (void)DDS_GuardCondition_set_trigger_value(
      impl->event_guards[RMW_EVENT_PUBLICATION_MATCHED], true);
  }
  if (callback != nullptr) {callback(user_data, 1U);}
}

void subscription_matched_listener(
  DDS_DataReader, const DDS_SubscriptionMatchedStatus * status, void * data)
{
  auto * impl = static_cast<SubscriptionImpl *>(data);
  rmw_event_callback_t callback = nullptr;
  const void * user_data = nullptr;
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    impl->subscription_matched.total_count = status->total_count;
    impl->subscription_matched.total_count_change += status->total_count_change;
    impl->subscription_matched.current_count = status->current_count;
    impl->subscription_matched.current_count_change += status->current_count_change;
    impl->subscription_matched_pending = true;
    callback = impl->event_callbacks[RMW_EVENT_SUBSCRIPTION_MATCHED];
    user_data = impl->event_user_data[RMW_EVENT_SUBSCRIPTION_MATCHED];
    (void)DDS_GuardCondition_set_trigger_value(
      impl->event_guards[RMW_EVENT_SUBSCRIPTION_MATCHED], true);
  }
  if (callback != nullptr) {callback(user_data, 1U);}
}

void liveliness_changed_listener(
  DDS_DataReader, const DDS_LivelinessChangedStatus * status, void * data)
{
  auto * impl = static_cast<SubscriptionImpl *>(data);
  rmw_event_callback_t callback = nullptr;
  const void * user_data = nullptr;
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    impl->liveliness_changed.alive_count = status->alive_count;
    impl->liveliness_changed.not_alive_count = status->not_alive_count;
    impl->liveliness_changed.alive_count_change += status->alive_count_change;
    impl->liveliness_changed.not_alive_count_change += status->not_alive_count_change;
    impl->liveliness_changed_pending = true;
    callback = impl->event_callbacks[RMW_EVENT_LIVELINESS_CHANGED];
    user_data = impl->event_user_data[RMW_EVENT_LIVELINESS_CHANGED];
    (void)DDS_GuardCondition_set_trigger_value(
      impl->event_guards[RMW_EVENT_LIVELINESS_CHANGED], true);
  }
  if (callback != nullptr) {callback(user_data, 1U);}
}

#define RMW_ZZDDS_PUBLISHER_COUNT_LISTENER(name, event_kind, field) \
  void name(DDS_DataWriter, const decltype(PublisherImpl::field) * status, void * data) \
  { \
    auto * impl = static_cast<PublisherImpl *>(data); \
    rmw_event_callback_t callback = nullptr; \
    const void * user_data = nullptr; \
    { \
      const std::lock_guard<std::mutex> lock(impl->event_mutex); \
      impl->field.total_count = status->total_count; \
      impl->field.total_count_change += status->total_count_change; \
      callback = impl->event_callbacks[event_kind]; \
      user_data = impl->event_user_data[event_kind]; \
      (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event_kind], true); \
    } \
    if (callback != nullptr) {callback(user_data, 1U);} \
  }

RMW_ZZDDS_PUBLISHER_COUNT_LISTENER(
  liveliness_lost_listener, RMW_EVENT_LIVELINESS_LOST, liveliness_lost)
RMW_ZZDDS_PUBLISHER_COUNT_LISTENER(
  offered_deadline_listener, RMW_EVENT_OFFERED_DEADLINE_MISSED, offered_deadline_missed)

void offered_incompatible_qos_listener(
  DDS_DataWriter, const DDS_OfferedIncompatibleQosStatus * status, void * data)
{
  auto * impl = static_cast<PublisherImpl *>(data);
  rmw_event_callback_t callback = nullptr;
  const void * user_data = nullptr;
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    impl->offered_incompatible_qos.total_count = status->total_count;
    impl->offered_incompatible_qos.total_count_change += status->total_count_change;
    impl->offered_incompatible_qos.last_policy_id = status->last_policy_id;
    callback = impl->event_callbacks[RMW_EVENT_OFFERED_QOS_INCOMPATIBLE];
    user_data = impl->event_user_data[RMW_EVENT_OFFERED_QOS_INCOMPATIBLE];
    (void)DDS_GuardCondition_set_trigger_value(
      impl->event_guards[RMW_EVENT_OFFERED_QOS_INCOMPATIBLE], true);
  }
  if (callback != nullptr) {callback(user_data, 1U);}
}

#define RMW_ZZDDS_SUBSCRIPTION_COUNT_LISTENER(name, status_type, event_kind, field) \
  void name(DDS_DataReader, const status_type * status, void * data) \
  { \
    auto * impl = static_cast<SubscriptionImpl *>(data); \
    rmw_event_callback_t callback = nullptr; \
    const void * user_data = nullptr; \
    { \
      const std::lock_guard<std::mutex> lock(impl->event_mutex); \
      impl->field.total_count = status->total_count; \
      impl->field.total_count_change += status->total_count_change; \
      callback = impl->event_callbacks[event_kind]; \
      user_data = impl->event_user_data[event_kind]; \
      (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event_kind], true); \
    } \
    if (callback != nullptr) {callback(user_data, 1U);} \
  }

RMW_ZZDDS_SUBSCRIPTION_COUNT_LISTENER(
  requested_deadline_listener, DDS_RequestedDeadlineMissedStatus,
  RMW_EVENT_REQUESTED_DEADLINE_MISSED, requested_deadline_missed)
RMW_ZZDDS_SUBSCRIPTION_COUNT_LISTENER(
  sample_lost_listener, DDS_SampleLostStatus, RMW_EVENT_MESSAGE_LOST, sample_lost)

void requested_incompatible_qos_listener(
  DDS_DataReader, const DDS_RequestedIncompatibleQosStatus * status, void * data)
{
  auto * impl = static_cast<SubscriptionImpl *>(data);
  rmw_event_callback_t callback = nullptr;
  const void * user_data = nullptr;
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    impl->requested_incompatible_qos.total_count = status->total_count;
    impl->requested_incompatible_qos.total_count_change += status->total_count_change;
    impl->requested_incompatible_qos.last_policy_id = status->last_policy_id;
    callback = impl->event_callbacks[RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE];
    user_data = impl->event_user_data[RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE];
    (void)DDS_GuardCondition_set_trigger_value(
      impl->event_guards[RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE], true);
  }
  if (callback != nullptr) {callback(user_data, 1U);}
}

#undef RMW_ZZDDS_PUBLISHER_COUNT_LISTENER
#undef RMW_ZZDDS_SUBSCRIPTION_COUNT_LISTENER


rmw_ret_t initialize_event(
  rmw_event_t * event, void * endpoint, DDS_StatusCondition condition, rmw_event_type_t type)
{
  if (event == nullptr || endpoint == nullptr || event->implementation_identifier != nullptr ||
    event->data != nullptr || event->event_type != RMW_EVENT_INVALID)
  {
    RMW_SET_ERROR_MSG("a zero-initialized event and valid endpoint are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  const DDS_StatusMask mask = event_mask(type);
  if (mask == DDS_STATUS_MASK_NONE) {
    RMW_SET_ERROR_MSG("event type is not supported by rmw_zzdds_cpp");
    return RMW_RET_UNSUPPORTED;
  }
  DDS_GuardCondition * guard = nullptr;
  if (publisher_event(type)) {
    guard = &static_cast<PublisherImpl *>(endpoint)->event_guards[type];
  } else {
    guard = &static_cast<SubscriptionImpl *>(endpoint)->event_guards[type];
  }
  if (*guard == nullptr) {*guard = zzdds_create_guardcondition();}
  if (*guard == nullptr) {
    RMW_SET_ERROR_MSG("failed to create the event callback guard condition");
    return RMW_RET_BAD_ALLOC;
  }
  const DDS_StatusMask enabled = DDS_StatusCondition_get_enabled_statuses(condition);
  if (DDS_StatusCondition_set_enabled_statuses(condition, enabled | mask) != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("failed to enable the zzdds event status");
    return RMW_RET_ERROR;
  }
  event->implementation_identifier = rmw_zzdds_cpp::identifier;
  event->data = endpoint;
  event->event_type = type;
  return RMW_RET_OK;
}
}  // namespace

namespace rmw_zzdds_cpp
{
DDS_Condition event_condition(const rmw_event_t * event)
{
  if (publisher_event(event->event_type)) {
    auto * impl = static_cast<PublisherImpl *>(event->data);
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    if (impl->event_callbacks[event->event_type] != nullptr) {
      return DDS_GuardCondition_as_DDS_Condition(impl->event_guards[event->event_type]);
    }
    return DDS_StatusCondition_as_DDS_Condition(DDS_DataWriter_get_statuscondition(impl->writer));
  }
  auto * impl = static_cast<SubscriptionImpl *>(event->data);
  const std::lock_guard<std::mutex> lock(impl->event_mutex);
  if (impl->event_callbacks[event->event_type] != nullptr) {
    return DDS_GuardCondition_as_DDS_Condition(impl->event_guards[event->event_type]);
  }
  return DDS_StatusCondition_as_DDS_Condition(DDS_DataReader_get_statuscondition(impl->reader));
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
  auto * impl = static_cast<PublisherImpl *>(publisher->data);
  return initialize_event(
    event, impl, DDS_DataWriter_get_statuscondition(impl->writer), type);
}

rmw_ret_t rmw_subscription_event_init(
  rmw_event_t * event, const rmw_subscription_t * subscription, rmw_event_type_t type)
{
  if (subscription == nullptr || subscription->data == nullptr) {return RMW_RET_INVALID_ARGUMENT;}
  if (subscription->implementation_identifier != rmw_zzdds_cpp::identifier) {
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (!subscription_event(type)) {return RMW_RET_UNSUPPORTED;}
  auto * impl = static_cast<SubscriptionImpl *>(subscription->data);
  return initialize_event(
    event, impl, DDS_DataReader_get_statuscondition(impl->reader), type);
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
  auto * output = static_cast<rmw_matched_status_t *>(event_info);
  if (event->event_type == RMW_EVENT_PUBLICATION_MATCHED) {
    DDS_PublicationMatchedStatus status{};
    auto * impl = static_cast<PublisherImpl *>(event->data);
    {
      const std::lock_guard<std::mutex> lock(impl->event_mutex);
      if (impl->event_callbacks[event->event_type] != nullptr) {
        status = impl->publication_matched;
        impl->publication_matched.total_count_change = 0;
        impl->publication_matched.current_count_change = 0;
        impl->publication_matched_pending = false;
        (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event->event_type], false);
      } else if (DDS_DataWriter_get_publication_matched_status(
          impl->writer, &status) != DDS_RETCODE_OK)
      {
        return RMW_RET_ERROR;
      }
    }
    output->total_count = static_cast<size_t>(status.total_count);
    output->total_count_change = static_cast<size_t>(status.total_count_change);
    output->current_count = static_cast<size_t>(status.current_count);
    output->current_count_change = status.current_count_change;
  } else if (event->event_type == RMW_EVENT_SUBSCRIPTION_MATCHED) {
    DDS_SubscriptionMatchedStatus status{};
    auto * impl = static_cast<SubscriptionImpl *>(event->data);
    {
      const std::lock_guard<std::mutex> lock(impl->event_mutex);
      if (impl->event_callbacks[event->event_type] != nullptr) {
        status = impl->subscription_matched;
        impl->subscription_matched.total_count_change = 0;
        impl->subscription_matched.current_count_change = 0;
        impl->subscription_matched_pending = false;
        (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event->event_type], false);
      } else if (DDS_DataReader_get_subscription_matched_status(
          impl->reader, &status) != DDS_RETCODE_OK)
      {
        return RMW_RET_ERROR;
      }
    }
    output->total_count = static_cast<size_t>(status.total_count);
    output->total_count_change = static_cast<size_t>(status.total_count_change);
    output->current_count = static_cast<size_t>(status.current_count);
    output->current_count_change = status.current_count_change;
  } else if (event->event_type == RMW_EVENT_LIVELINESS_CHANGED) {
    DDS_LivelinessChangedStatus status{};
    auto * impl = static_cast<SubscriptionImpl *>(event->data);
    {
      const std::lock_guard<std::mutex> lock(impl->event_mutex);
      if (impl->event_callbacks[event->event_type] != nullptr) {
        status = impl->liveliness_changed;
        impl->liveliness_changed.alive_count_change = 0;
        impl->liveliness_changed.not_alive_count_change = 0;
        impl->liveliness_changed_pending = false;
        (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event->event_type], false);
      } else if (DDS_DataReader_get_liveliness_changed_status(
          impl->reader, &status) != DDS_RETCODE_OK)
      {
        return RMW_RET_ERROR;
      }
    }
    auto * liveliness = static_cast<rmw_liveliness_changed_status_t *>(event_info);
    liveliness->alive_count = status.alive_count;
    liveliness->not_alive_count = status.not_alive_count;
    liveliness->alive_count_change = status.alive_count_change;
    liveliness->not_alive_count_change = status.not_alive_count_change;
  } else if (event->event_type == RMW_EVENT_LIVELINESS_LOST) {
    auto * impl = static_cast<PublisherImpl *>(event->data);
    DDS_LivelinessLostStatus status{};
    {
      const std::lock_guard<std::mutex> lock(impl->event_mutex);
      if (impl->event_callbacks[event->event_type] != nullptr) {
        status = impl->liveliness_lost;
        impl->liveliness_lost.total_count_change = 0;
        (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event->event_type], false);
      } else if (DDS_DataWriter_get_liveliness_lost_status(impl->writer, &status) != DDS_RETCODE_OK) {
        return RMW_RET_ERROR;
      }
    }
    auto * result = static_cast<rmw_liveliness_lost_status_t *>(event_info);
    result->total_count = status.total_count;
    result->total_count_change = status.total_count_change;
  } else if (event->event_type == RMW_EVENT_OFFERED_DEADLINE_MISSED) {
    auto * impl = static_cast<PublisherImpl *>(event->data);
    DDS_OfferedDeadlineMissedStatus status{};
    {
      const std::lock_guard<std::mutex> lock(impl->event_mutex);
      if (impl->event_callbacks[event->event_type] != nullptr) {
        status = impl->offered_deadline_missed;
        impl->offered_deadline_missed.total_count_change = 0;
        (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event->event_type], false);
      } else if (DDS_DataWriter_get_offered_deadline_missed_status(
          impl->writer, &status) != DDS_RETCODE_OK)
      {
        return RMW_RET_ERROR;
      }
    }
    auto * result = static_cast<rmw_offered_deadline_missed_status_t *>(event_info);
    result->total_count = status.total_count;
    result->total_count_change = status.total_count_change;
  } else if (event->event_type == RMW_EVENT_REQUESTED_DEADLINE_MISSED) {
    auto * impl = static_cast<SubscriptionImpl *>(event->data);
    DDS_RequestedDeadlineMissedStatus status{};
    {
      const std::lock_guard<std::mutex> lock(impl->event_mutex);
      if (impl->event_callbacks[event->event_type] != nullptr) {
        status = impl->requested_deadline_missed;
        impl->requested_deadline_missed.total_count_change = 0;
        (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event->event_type], false);
      } else if (DDS_DataReader_get_requested_deadline_missed_status(
          impl->reader, &status) != DDS_RETCODE_OK)
      {
        return RMW_RET_ERROR;
      }
    }
    auto * result = static_cast<rmw_requested_deadline_missed_status_t *>(event_info);
    result->total_count = status.total_count;
    result->total_count_change = status.total_count_change;
  } else if (event->event_type == RMW_EVENT_MESSAGE_LOST) {
    auto * impl = static_cast<SubscriptionImpl *>(event->data);
    DDS_SampleLostStatus status{};
    {
      const std::lock_guard<std::mutex> lock(impl->event_mutex);
      if (impl->event_callbacks[event->event_type] != nullptr) {
        status = impl->sample_lost;
        impl->sample_lost.total_count_change = 0;
        (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event->event_type], false);
      } else if (DDS_DataReader_get_sample_lost_status(impl->reader, &status) != DDS_RETCODE_OK) {
        return RMW_RET_ERROR;
      }
    }
    auto * result = static_cast<rmw_message_lost_status_t *>(event_info);
    result->total_count = static_cast<size_t>(status.total_count);
    result->total_count_change = static_cast<size_t>(status.total_count_change);
  } else if (event->event_type == RMW_EVENT_OFFERED_QOS_INCOMPATIBLE) {
    auto * impl = static_cast<PublisherImpl *>(event->data);
    DDS_OfferedIncompatibleQosStatus status{};
    {
      const std::lock_guard<std::mutex> lock(impl->event_mutex);
      if (impl->event_callbacks[event->event_type] != nullptr) {
        status.total_count = impl->offered_incompatible_qos.total_count;
        status.total_count_change = impl->offered_incompatible_qos.total_count_change;
        status.last_policy_id = impl->offered_incompatible_qos.last_policy_id;
        impl->offered_incompatible_qos.total_count_change = 0;
        (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event->event_type], false);
      } else if (DDS_DataWriter_get_offered_incompatible_qos_status(
          impl->writer, &status) != DDS_RETCODE_OK)
      {
        return RMW_RET_ERROR;
      }
    }
    auto * result = static_cast<rmw_offered_qos_incompatible_event_status_t *>(event_info);
    result->total_count = status.total_count;
    result->total_count_change = status.total_count_change;
    result->last_policy_kind = qos_policy(status.last_policy_id);
  } else if (event->event_type == RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE) {
    auto * impl = static_cast<SubscriptionImpl *>(event->data);
    DDS_RequestedIncompatibleQosStatus status{};
    {
      const std::lock_guard<std::mutex> lock(impl->event_mutex);
      if (impl->event_callbacks[event->event_type] != nullptr) {
        status.total_count = impl->requested_incompatible_qos.total_count;
        status.total_count_change = impl->requested_incompatible_qos.total_count_change;
        status.last_policy_id = impl->requested_incompatible_qos.last_policy_id;
        impl->requested_incompatible_qos.total_count_change = 0;
        (void)DDS_GuardCondition_set_trigger_value(impl->event_guards[event->event_type], false);
      } else if (DDS_DataReader_get_requested_incompatible_qos_status(
          impl->reader, &status) != DDS_RETCODE_OK)
      {
        return RMW_RET_ERROR;
      }
    }
    auto * result = static_cast<rmw_requested_qos_incompatible_event_status_t *>(event_info);
    result->total_count = status.total_count;
    result->total_count_change = status.total_count_change;
    result->last_policy_kind = qos_policy(status.last_policy_id);
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
        if (impl->event_callbacks[type] != nullptr) {mask |= event_mask(type);}
      }
    }
    return DDS_DataWriter_set_listener(
      impl->writer, mask == DDS_STATUS_MASK_NONE ? nullptr : &listener, mask) == DDS_RETCODE_OK ?
           RMW_RET_OK : RMW_RET_ERROR;
  }
  auto * impl = static_cast<SubscriptionImpl *>(event->data);
  DDS_StatusMask mask = DDS_STATUS_MASK_NONE;
  {
    const std::lock_guard<std::mutex> lock(impl->event_mutex);
    impl->event_callbacks[event->event_type] = callback;
    impl->event_user_data[event->event_type] = user_data;
    if (impl->event_callbacks[RMW_EVENT_SUBSCRIPTION_MATCHED] != nullptr) {
      mask |= DDS_SUBSCRIPTION_MATCHED_STATUS;
    }
    if (impl->event_callbacks[RMW_EVENT_LIVELINESS_CHANGED] != nullptr) {
      mask |= DDS_LIVELINESS_CHANGED_STATUS;
    }
    for (rmw_event_type_t type : {
        RMW_EVENT_REQUESTED_DEADLINE_MISSED, RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE,
        RMW_EVENT_MESSAGE_LOST})
    {
      if (impl->event_callbacks[type] != nullptr) {mask |= event_mask(type);}
    }
  }
  DDS_DataReaderListener listener{};
  listener.listener_data = impl;
  listener.on_subscription_matched = subscription_matched_listener;
  listener.on_liveliness_changed = liveliness_changed_listener;
  listener.on_requested_deadline_missed = requested_deadline_listener;
  listener.on_requested_incompatible_qos = requested_incompatible_qos_listener;
  listener.on_sample_lost = sample_lost_listener;
  return DDS_DataReader_set_listener(
    impl->reader, mask == DDS_STATUS_MASK_NONE ? nullptr : &listener, mask) == DDS_RETCODE_OK ?
         RMW_RET_OK : RMW_RET_ERROR;
}

}
