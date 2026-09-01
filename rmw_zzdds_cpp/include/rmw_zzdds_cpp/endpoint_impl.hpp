#ifndef RMW_ZZDDS_CPP__ENDPOINT_IMPL_HPP_
#define RMW_ZZDDS_CPP__ENDPOINT_IMPL_HPP_

#include <array>
#include <mutex>
#include <unordered_set>

#include "rmw/types.h"
#include "rosidl_typesupport_zzdds_cpp/message_type_support.hpp"
#include "zzdds_c.h"

#include "rmw_zzdds_cpp/context_impl.hpp"

namespace rmw_zzdds_cpp
{

using MessageCallbacks = rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t;

struct PublisherImpl final
{
  PublisherImpl(
    ContextImpl * c, const rmw_node_t * n, DDS_Topic t, DDS_DataWriter w, const MessageCallbacks * cb,
    rosidl_type_hash_t h, rmw_qos_profile_t q)
  : context(c), node(n), topic(t), writer(w), callbacks(cb), type_hash(h), qos(q) {}
  ContextImpl * context;
  const rmw_node_t * node;
  DDS_Topic topic;
  DDS_DataWriter writer;
  const MessageCallbacks * callbacks;
  rosidl_type_hash_t type_hash;
  rmw_qos_profile_t qos;
  std::array<uint8_t, 16U> rtps_guid{};
  std::unordered_set<void *> loaned_messages;
  std::mutex event_mutex;
  std::array<DDS_GuardCondition, RMW_EVENT_TYPE_MAX> event_guards{};
  std::array<rmw_event_callback_t, RMW_EVENT_TYPE_MAX> event_callbacks{};
  std::array<const void *, RMW_EVENT_TYPE_MAX> event_user_data{};
  DDS_PublicationMatchedStatus publication_matched{};
  DDS_LivelinessLostStatus liveliness_lost{};
  DDS_OfferedDeadlineMissedStatus offered_deadline_missed{};
  DDS_OfferedIncompatibleQosStatus offered_incompatible_qos{};
  bool publication_matched_pending{false};
  bool is_service_endpoint{false};
};

struct SubscriptionImpl final
{
  SubscriptionImpl(
    ContextImpl * c, const rmw_node_t * n, DDS_Topic t, DDS_ContentFilteredTopic f, DDS_DataReader r,
    DDS_ReadCondition condition, const MessageCallbacks * cb, rmw_qos_profile_t q,
    rosidl_type_hash_t h, bool ignore_local)
  : context(c), node(n), topic(t), filtered_topic(f), reader(r), read_condition(condition),
    callbacks(cb), qos(q), type_hash(h), ignore_local_publications(ignore_local) {}
  ContextImpl * context;
  const rmw_node_t * node;
  DDS_Topic topic;
  DDS_ContentFilteredTopic filtered_topic;
  DDS_DataReader reader;
  DDS_ReadCondition read_condition;
  const MessageCallbacks * callbacks;
  rmw_qos_profile_t qos;
  rosidl_type_hash_t type_hash;
  bool ignore_local_publications;
  std::unordered_set<void *> loaned_messages;
  std::mutex event_mutex;
  std::array<DDS_GuardCondition, RMW_EVENT_TYPE_MAX> event_guards{};
  std::array<rmw_event_callback_t, RMW_EVENT_TYPE_MAX> event_callbacks{};
  std::array<const void *, RMW_EVENT_TYPE_MAX> event_user_data{};
  DDS_SubscriptionMatchedStatus subscription_matched{};
  DDS_LivelinessChangedStatus liveliness_changed{};
  DDS_RequestedDeadlineMissedStatus requested_deadline_missed{};
  DDS_RequestedIncompatibleQosStatus requested_incompatible_qos{};
  DDS_SampleLostStatus sample_lost{};
  bool subscription_matched_pending{false};
  bool liveliness_changed_pending{false};
  bool is_service_endpoint{false};
};

}  // namespace rmw_zzdds_cpp

#endif  // RMW_ZZDDS_CPP__ENDPOINT_IMPL_HPP_
