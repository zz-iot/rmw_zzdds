#include "rmw_zzdds_cpp/graph_discovery.hpp"

#include <array>
#include <cstring>
#include <type_traits>

#include "rmw_dds_common/qos.hpp"
#include "rmw_dds_common/msg/participant_entities_info.hpp"
#include "rmw/rmw.h"
#include "rosidl_runtime_c/type_hash.h"
#include "zidl_cdr.h"

#include "rmw_zzdds_cpp/context_impl.hpp"
#include "rmw_zzdds_cpp/identifier.hpp"
#include "rosidl_typesupport_zzdds_cpp/message_type_support.hpp"
#include "rmw_dds_common/msg/detail/participant_entities_info__rosidl_typesupport_zzdds_cpp.hpp"

namespace rmw_zzdds_cpp
{
namespace
{
using GuidBytes = std::array<uint8_t, 16U>;

rmw_gid_t endpoint_gid(const DDS_OctetSeq & hashes, uint32_t index)
{
  rmw_gid_t gid{};
  gid.implementation_identifier = identifier;
  constexpr size_t guid_size = 16U;
  const size_t offset = static_cast<size_t>(index) * guid_size;
  if (hashes._buffer != nullptr && offset + guid_size <= hashes._length) {
    std::memcpy(gid.data, hashes._buffer + offset, guid_size);
  }
  return gid;
}

rmw_gid_t participant_gid(const DDS_OctetSeq & hashes, uint32_t index)
{
  rmw_gid_t gid{};
  gid.implementation_identifier = identifier;
  constexpr size_t guid_size = 16U;
  const size_t offset = static_cast<size_t>(index) * guid_size;
  if (hashes._buffer != nullptr && offset + guid_size <= hashes._length) {
    std::memcpy(gid.data, hashes._buffer + offset, 12U);
    gid.data[12] = 0x00U;
    gid.data[13] = 0x00U;
    gid.data[14] = 0x01U;
    gid.data[15] = 0xc1U;
  }
  return gid;
}

template<typename BuiltinDataT>
bool deserialize(const DDS_OctetSeq & payload, BuiltinDataT & data)
{
  ZidlCdrReader reader{};
  if (payload._buffer == nullptr ||
    zidl_cdr_reader_init(&reader, payload._buffer, payload._length) != ZIDL_CDR_OK)
  {
    return false;
  }
  if constexpr (std::is_same_v<BuiltinDataT, DDS_PublicationBuiltinTopicData>) {
    return DDS_PublicationBuiltinTopicData_deserialize(&reader, &data) == ZIDL_CDR_OK;
  } else {
    return DDS_SubscriptionBuiltinTopicData_deserialize(&reader, &data) == ZIDL_CDR_OK;
  }
}

template<typename BuiltinDataT>
void free_data(BuiltinDataT & data)
{
  if constexpr (std::is_same_v<BuiltinDataT, DDS_PublicationBuiltinTopicData>) {
    DDS_PublicationBuiltinTopicData_free(&data);
  } else {
    DDS_SubscriptionBuiltinTopicData_free(&data);
  }
}

template<typename BuiltinDataT>
rmw_ret_t consume_reader(
  ContextImpl * context, DDS_DataReader reader, bool readers,
  std::unordered_map<DDS_InstanceHandle_t, rmw_gid_t> & instances,
  bool & changed)
{
  DDS_OctetSeqSeq payloads{};
  DDS_OctetSeq hashes{};
  DDS_SampleInfoSeq infos{};
  const DDS_ReturnCode_t take_result = DDS_DataReader_take_raw(
    reader, &payloads, &hashes, &infos, DDS_HANDLE_NIL, nullptr,
    DDS_ANY_SAMPLE_STATE, DDS_ANY_VIEW_STATE, DDS_ANY_INSTANCE_STATE, DDS_LENGTH_UNLIMITED);
  if (take_result == DDS_RETCODE_NO_DATA) {
    return RMW_RET_OK;
  }
  if (take_result != DDS_RETCODE_OK) {
    return RMW_RET_ERROR;
  }

  rmw_ret_t result = RMW_RET_OK;
  for (uint32_t i = 0; i < infos._length; ++i) {
    const DDS_SampleInfo & info = infos._buffer[i];
    if (!info.valid_data) {
      const auto found = instances.find(info.instance_handle);
      if (found != instances.end()) {
        changed = context->common.graph_cache.remove_entity(found->second, readers) || changed;
        instances.erase(found);
      }
      continue;
    }
    if (i >= payloads._length) {
      result = RMW_RET_ERROR;
      break;
    }

    BuiltinDataT data{};
    if (!deserialize(payloads._buffer[i], data)) {
      result = RMW_RET_ERROR;
      break;
    }
    const rmw_gid_t endpoint = endpoint_gid(hashes, i);
    const rmw_gid_t participant = participant_gid(hashes, i);
    if (data.topic_name != nullptr && std::strcmp(data.topic_name, "rt/ros_discovery_info") == 0) {
      free_data(data);
      continue;
    }
    rosidl_type_hash_t type_hash = rosidl_get_zero_initialized_type_hash();
    rosidl_type_hash_t service_type_hash = rosidl_get_zero_initialized_type_hash();
    bool has_service_type_hash = false;
    const uint8_t * user_data = data.user_data.value._buffer;
    const size_t user_data_size = data.user_data.value._length;
    if (user_data_size != 0U &&
      rmw_dds_common::parse_type_hash_from_user_data(
        user_data, user_data_size, type_hash) != RMW_RET_OK)
    {
      free_data(data);
      result = RMW_RET_ERROR;
      break;
    }
    if (user_data_size != 0U) {
      const rmw_ret_t hash_result = rmw_dds_common::parse_sertype_hash_from_user_data(
        user_data, user_data_size, service_type_hash);
      if (hash_result == RMW_RET_OK) {
        has_service_type_hash = true;
      } else if (hash_result != RMW_RET_UNSUPPORTED) {
        free_data(data);
        result = RMW_RET_ERROR;
        break;
      }
    }

    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.reliability = data.reliability.kind ==
      DDS_ReliabilityQosPolicyKind_RELIABLE_RELIABILITY_QOS ?
      RMW_QOS_POLICY_RELIABILITY_RELIABLE : RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    qos.durability = data.durability.kind ==
      DDS_DurabilityQosPolicyKind_VOLATILE_DURABILITY_QOS ?
      RMW_QOS_POLICY_DURABILITY_VOLATILE : RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
    qos.liveliness = data.liveliness.kind ==
      DDS_LivelinessQosPolicyKind_AUTOMATIC_LIVELINESS_QOS ?
      RMW_QOS_POLICY_LIVELINESS_AUTOMATIC : RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC;

    context->common.graph_cache.add_participant(participant, "");
    changed = context->common.graph_cache.add_entity(
      endpoint, data.topic_name == nullptr ? "" : data.topic_name,
      data.type_name == nullptr ? "" : data.type_name, type_hash, participant, qos, readers,
      has_service_type_hash ? &service_type_hash : nullptr) || changed;
    instances[info.instance_handle] = endpoint;
    free_data(data);
  }

  const DDS_ReturnCode_t return_result =
    DDS_DataReader_return_loan_raw(reader, &payloads, &hashes, &infos);
  return result == RMW_RET_OK && return_result == DDS_RETCODE_OK ? RMW_RET_OK : RMW_RET_ERROR;
}
}  // namespace

rmw_ret_t refresh_discovered_endpoints(ContextImpl * context)
{
  if (context == nullptr) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  const DDS_Subscriber builtin = DDS_DomainParticipant_get_builtin_subscriber(
    context->dds.participant());
  if (builtin == nullptr) {
    return RMW_RET_ERROR;
  }
  const DDS_DataReader publications = DDS_Subscriber_lookup_datareader(builtin, "DCPSPublication");
  const DDS_DataReader subscriptions =
    DDS_Subscriber_lookup_datareader(builtin, "DCPSSubscription");
  if (publications == nullptr || subscriptions == nullptr) {
    return RMW_RET_ERROR;
  }

  const std::lock_guard<std::mutex> lock(context->mutex);
  bool changed = false;
  rmw_ret_t result = consume_reader<DDS_PublicationBuiltinTopicData>(
    context, publications, false, context->discovered_writer_instances, changed);
  if (result == RMW_RET_OK) {
    result = consume_reader<DDS_SubscriptionBuiltinTopicData>(
      context, subscriptions, true, context->discovered_reader_instances, changed);
  }
  if (result == RMW_RET_OK && changed) {
    context->notify_graph_change();
  }
  return result;
}

rmw_ret_t initialize_graph_channel(rmw_context_t * context)
{
  if (context == nullptr || context->impl == nullptr) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = reinterpret_cast<ContextImpl *>(context->impl);
  impl->suppress_graph_updates = true;
  impl->graph_node = rmw_create_node(context, "__rmw_zzdds_graph", "/");
  if (impl->graph_node == nullptr) {
    impl->suppress_graph_updates = false;
    return RMW_RET_ERROR;
  }

  const auto * type_support =
    rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
    rmw_dds_common::msg::ParticipantEntitiesInfo>();
  rmw_qos_profile_t qos = rmw_qos_profile_default;
  qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  qos.depth = 1U;
  qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
  qos.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
  const rmw_publisher_options_t publisher_options = rmw_get_default_publisher_options();
  const rmw_subscription_options_t subscription_options = rmw_get_default_subscription_options();
  impl->common.pub = rmw_create_publisher(
    impl->graph_node, type_support, "/ros_discovery_info", &qos, &publisher_options);
  impl->common.sub = rmw_create_subscription(
    impl->graph_node, type_support, "/ros_discovery_info", &qos, &subscription_options);
  impl->suppress_graph_updates = false;
  if (impl->common.pub == nullptr || impl->common.sub == nullptr) {
    return finalize_graph_channel(context);
  }
  impl->common.listener_thread_gc = rmw_create_guard_condition(context);
  if (impl->common.listener_thread_gc == nullptr) {
    return finalize_graph_channel(context);
  }
  impl->common.publish_callback = [](const rmw_publisher_t * publisher, const void * message) {
      return rmw_publish(publisher, message, nullptr);
    };
  impl->common.thread_is_running.store(true);
  try {
    impl->common.listener_thread = std::thread([impl]() {
        rmw_wait_set_t * wait_set = rmw_create_wait_set(impl->graph_node->context, 2U);
        if (wait_set == nullptr) {
          impl->common.thread_is_running.store(false);
          return;
        }
        while (impl->common.thread_is_running.load()) {
          void * subscription_entries[] = {impl->common.sub->data};
          void * guard_entries[] = {impl->common.listener_thread_gc->data};
          rmw_subscriptions_t subscriptions{1U, subscription_entries};
          rmw_guard_conditions_t guards{1U, guard_entries};
          if (rmw_wait(
              &subscriptions, &guards, nullptr, nullptr, nullptr, wait_set, nullptr) !=
            RMW_RET_OK)
          {
            break;
          }
          if (subscription_entries[0] != nullptr) {
            rmw_dds_common::msg::ParticipantEntitiesInfo message;
            bool taken = false;
            if (rmw_take(impl->common.sub, &message, &taken, nullptr) == RMW_RET_OK && taken) {
              if (std::memcmp(
                  message.gid.data.data(), impl->common.gid.data,
                  message.gid.data.size()) != 0)
              {
                impl->common.graph_cache.update_participant_entities(message);
                impl->notify_graph_change();
              }
            }
          }
        }
        const rmw_ret_t destroy_result = rmw_destroy_wait_set(wait_set);
        (void)destroy_result;
      });
  } catch (...) {
    impl->common.thread_is_running.store(false);
    return finalize_graph_channel(context);
  }
  return RMW_RET_OK;
}

void stop_graph_listener(ContextImpl * context) noexcept
{
  if (context == nullptr) {
    return;
  }
  context->common.thread_is_running.store(false);
  if (context->common.listener_thread_gc != nullptr) {
    const rmw_ret_t trigger_result =
      rmw_trigger_guard_condition(context->common.listener_thread_gc);
    (void)trigger_result;
  }
  if (context->common.listener_thread.joinable()) {
    context->common.listener_thread.join();
  }
}

rmw_ret_t finalize_graph_channel(rmw_context_t * context) noexcept
{
  if (context == nullptr || context->impl == nullptr) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = reinterpret_cast<ContextImpl *>(context->impl);
  stop_graph_listener(impl);
  impl->common.publish_callback = nullptr;
  impl->suppress_graph_updates = true;
  rmw_ret_t result = RMW_RET_OK;
  if (impl->common.sub != nullptr) {
    result = rmw_destroy_subscription(impl->graph_node, impl->common.sub);
    impl->common.sub = nullptr;
  }
  if (impl->common.pub != nullptr) {
    const rmw_ret_t ret = rmw_destroy_publisher(impl->graph_node, impl->common.pub);
    if (result == RMW_RET_OK) {
      result = ret;
    }
    impl->common.pub = nullptr;
  }
  if (impl->common.listener_thread_gc != nullptr) {
    const rmw_ret_t ret = rmw_destroy_guard_condition(impl->common.listener_thread_gc);
    if (result == RMW_RET_OK) {
      result = ret;
    }
    impl->common.listener_thread_gc = nullptr;
  }
  if (impl->graph_node != nullptr) {
    const rmw_ret_t ret = rmw_destroy_node(impl->graph_node);
    if (result == RMW_RET_OK) {
      result = ret;
    }
    impl->graph_node = nullptr;
  }
  impl->suppress_graph_updates = false;
  return result;
}
}  // namespace rmw_zzdds_cpp
