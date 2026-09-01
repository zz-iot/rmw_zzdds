#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "rcutils/types/uint8_array.h"
#include "rcutils/time.h"
#include "rmw/error_handling.h"
#include "rmw/rmw.h"
#include "rmw/validate_full_topic_name.h"
#include "rmw_dds_common/qos.hpp"

#include "rmw_zzdds_cpp/allocator.hpp"
#include "rmw_zzdds_cpp/endpoint_impl.hpp"
#include "rmw_zzdds_cpp/event_impl.hpp"
#include "rmw_zzdds_cpp/identifier.hpp"
#include "rmw_zzdds_cpp/node_impl.hpp"
#include "rmw_zzdds_cpp/service_type_hash.hpp"
#include "rmw_zzdds_cpp/type_support.hpp"

namespace rmw_zzdds_cpp
{
namespace
{
thread_local const rosidl_type_hash_t * current_service_type_hash = nullptr;
}

const rosidl_type_hash_t * service_type_hash_override() noexcept
{
  return current_service_type_hash;
}

void set_service_type_hash_override(const rosidl_type_hash_t * type_hash) noexcept
{
  current_service_type_hash = type_hash;
}
}  // namespace rmw_zzdds_cpp

namespace
{

using rmw_zzdds_cpp::ContextImpl;
using rmw_zzdds_cpp::PublisherImpl;
using rmw_zzdds_cpp::SubscriptionImpl;

bool valid_node(const rmw_node_t * node)
{
  return node != nullptr && node->implementation_identifier == rmw_zzdds_cpp::identifier &&
         node->data != nullptr && node->context != nullptr && node->context->impl != nullptr;
}

bool validate_topic(const char * topic_name, bool native)
{
  if (topic_name == nullptr || topic_name[0] == '\0') {
    RMW_SET_ERROR_MSG("a non-empty topic name is required");
    return false;
  }
  if (native) {
    return true;
  }
  int validation = RMW_TOPIC_VALID;
  if (rmw_validate_full_topic_name(topic_name, &validation, nullptr) != RMW_RET_OK) {
    return false;
  }
  if (validation != RMW_TOPIC_VALID) {
    RMW_SET_ERROR_MSG(rmw_full_topic_name_validation_result_string(validation));
    return false;
  }
  return true;
}

bool dds_topic_name(const char * ros_name, bool native, std::array<char, 264> & output)
{
  const char * prefix = native ? "" : "rt";
  const size_t prefix_size = std::strlen(prefix);
  const size_t name_size = std::strlen(ros_name);
  if (prefix_size + name_size + 1U > output.size()) {
    RMW_SET_ERROR_MSG("DDS topic name is too long");
    return false;
  }
  std::memcpy(output.data(), prefix, prefix_size);
  std::memcpy(output.data() + prefix_size, ros_name, name_size + 1U);
  return true;
}

template<typename Qos>
bool apply_qos(const rmw_qos_profile_t & source, Qos & target)
{
  switch (source.reliability) {
    case RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT:
      break;
    case RMW_QOS_POLICY_RELIABILITY_RELIABLE:
      target.reliability.kind = DDS_ReliabilityQosPolicyKind_RELIABLE_RELIABILITY_QOS;
      break;
    case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT:
      target.reliability.kind = DDS_ReliabilityQosPolicyKind_BEST_EFFORT_RELIABILITY_QOS;
      break;
    default:
      RMW_SET_ERROR_MSG("unsupported or unspecified reliability policy");
      return false;
  }
  switch (source.durability) {
    case RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT:
      break;
    case RMW_QOS_POLICY_DURABILITY_VOLATILE:
      target.durability.kind = DDS_DurabilityQosPolicyKind_VOLATILE_DURABILITY_QOS;
      break;
    case RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL:
      target.durability.kind = DDS_DurabilityQosPolicyKind_TRANSIENT_LOCAL_DURABILITY_QOS;
      break;
    default:
      RMW_SET_ERROR_MSG("unsupported or unspecified durability policy");
      return false;
  }
  switch (source.history) {
    case RMW_QOS_POLICY_HISTORY_SYSTEM_DEFAULT:
      break;
    case RMW_QOS_POLICY_HISTORY_KEEP_ALL:
      target.history.kind = DDS_HistoryQosPolicyKind_KEEP_ALL_HISTORY_QOS;
      break;
    case RMW_QOS_POLICY_HISTORY_KEEP_LAST:
      if (source.depth > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        RMW_SET_ERROR_MSG("QoS history depth exceeds the DDS representation");
        return false;
      }
      target.history.kind = DDS_HistoryQosPolicyKind_KEEP_LAST_HISTORY_QOS;
      target.history.depth = static_cast<int32_t>(source.depth);
      break;
    default:
      RMW_SET_ERROR_MSG("unsupported or unspecified history policy");
      return false;
  }
  switch (source.liveliness) {
    case RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT:
    case RMW_QOS_POLICY_LIVELINESS_AUTOMATIC:
      target.liveliness.kind = DDS_LivelinessQosPolicyKind_AUTOMATIC_LIVELINESS_QOS;
      break;
    case RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC:
      target.liveliness.kind = DDS_LivelinessQosPolicyKind_MANUAL_BY_TOPIC_LIVELINESS_QOS;
      break;
    default:
      RMW_SET_ERROR_MSG("unsupported or unspecified liveliness policy");
      return false;
  }
  return true;
}

template<typename Qos>
rmw_qos_profile_t resolved_qos(const rmw_qos_profile_t & requested, const Qos & applied)
{
  rmw_qos_profile_t result = requested;
  result.reliability =
    applied.reliability.kind == DDS_ReliabilityQosPolicyKind_RELIABLE_RELIABILITY_QOS ?
    RMW_QOS_POLICY_RELIABILITY_RELIABLE : RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
  result.durability =
    applied.durability.kind == DDS_DurabilityQosPolicyKind_TRANSIENT_LOCAL_DURABILITY_QOS ?
    RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL : RMW_QOS_POLICY_DURABILITY_VOLATILE;
  if (applied.history.kind == DDS_HistoryQosPolicyKind_KEEP_ALL_HISTORY_QOS) {
    result.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
    result.depth = 0U;
  } else {
    result.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    result.depth = applied.history.depth < 0 ? 0U : static_cast<size_t>(applied.history.depth);
  }
  result.liveliness =
    applied.liveliness.kind == DDS_LivelinessQosPolicyKind_MANUAL_BY_TOPIC_LIVELINESS_QOS ?
    RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC : RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
  return result;
}

rmw_ret_t validate_publisher(const rmw_publisher_t * publisher)
{
  if (publisher == nullptr || publisher->data == nullptr) {
    RMW_SET_ERROR_MSG("a valid publisher is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (publisher->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("publisher belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  return RMW_RET_OK;
}

rmw_ret_t validate_subscription(const rmw_subscription_t * subscription)
{
  if (subscription == nullptr || subscription->data == nullptr) {
    RMW_SET_ERROR_MSG("a valid subscription is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (subscription->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("subscription belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  return RMW_RET_OK;
}

rmw_ret_t write_serialized(
  PublisherImpl * impl, const uint8_t * data, size_t size,
  const void * ros_message)
{
  std::array<uint8_t, 16> key_hash{};
  if (impl->callbacks->has_key) {
    if (ros_message == nullptr || impl->callbacks->compute_key_hash == nullptr ||
      !impl->callbacks->compute_key_hash(ros_message, key_hash.data()))
    {
      RMW_SET_ERROR_MSG("failed to compute the DDS key hash");
      return RMW_RET_ERROR;
    }
  }
  if (size > UINT32_MAX) {
    RMW_SET_ERROR_MSG("serialized sample exceeds the DDS sequence representation");
    return RMW_RET_ERROR;
  }
  DDS_OctetSeq dds_key{
    static_cast<uint32_t>(key_hash.size()), static_cast<uint32_t>(key_hash.size()),
    key_hash.data(), false};
  DDS_OctetSeq payload{
    static_cast<uint32_t>(size), static_cast<uint32_t>(size),
    const_cast<uint8_t *>(data), false};
  const DDS_Time_t source_timestamp{DDS_TIME_INVALID_SEC, DDS_TIME_INVALID_NSEC};
  const DDS_ReturnCode_t result = DDS_DataWriter_write_raw(
    impl->writer, &dds_key, DDS_HANDLE_NIL, &payload,
    DDS_WriteKind_ALIVE_WRITE_KIND, &source_timestamp);
  if (result != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("zzdds failed to write the serialized sample");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

struct RawLoan final
{
  DDS_OctetSeqSeq payloads{};
  DDS_OctetSeq key_hashes{};
  DDS_SampleInfoSeq infos{};
  const uint8_t * data{};
  size_t data_len{};
};

DDS_ReturnCode_t return_raw_loan(DDS_DataReader reader, RawLoan * loan)
{
  const DDS_ReturnCode_t result = DDS_DataReader_return_loan_raw(
    reader, &loan->payloads, &loan->key_hashes, &loan->infos);
  *loan = {};
  return result;
}

bool guid_from_writer(DDS_DataWriter writer, rmw_gid_t * gid)
{
  static_assert(16U <= RMW_GID_STORAGE_SIZE);
  zzdds_RtpsGuid rtps_guid{};
  const zzdds_DataWriter extension = DDS_DataWriter_as_zzdds_DataWriter(writer);
  if (extension == nullptr || zzdds_DataWriter_get_rtps_guid(extension, &rtps_guid) != DDS_RETCODE_OK) {
    return false;
  }
  *gid = {};
  gid->implementation_identifier = rmw_zzdds_cpp::identifier;
  std::memcpy(gid->data, rtps_guid.value, sizeof(rtps_guid.value));
  return true;
}

bool guid_from_reader(DDS_DataReader reader, rmw_gid_t * gid)
{
  static_assert(16U <= RMW_GID_STORAGE_SIZE);
  zzdds_RtpsGuid rtps_guid{};
  const zzdds_DataReader extension = DDS_DataReader_as_zzdds_DataReader(reader);
  if (extension == nullptr || zzdds_DataReader_get_rtps_guid(extension, &rtps_guid) != DDS_RETCODE_OK) {
    return false;
  }
  *gid = {};
  gid->implementation_identifier = rmw_zzdds_cpp::identifier;
  std::memcpy(gid->data, rtps_guid.value, sizeof(rtps_guid.value));
  return true;
}

bool guid_from_publication(
  DDS_DataReader reader, DDS_InstanceHandle_t publication_handle, rmw_gid_t * gid)
{
  static_assert(16U <= RMW_GID_STORAGE_SIZE);
  zzdds_RtpsGuid rtps_guid{};
  const zzdds_DataReader extension = DDS_DataReader_as_zzdds_DataReader(reader);
  if (extension == nullptr ||
    zzdds_DataReader_get_matched_publication_rtps_guid(
      extension, publication_handle, &rtps_guid) !=
    DDS_RETCODE_OK)
  {
    return false;
  }
  *gid = {};
  gid->implementation_identifier = rmw_zzdds_cpp::identifier;
  std::memcpy(gid->data, rtps_guid.value, sizeof(rtps_guid.value));
  return true;
}

rosidl_type_hash_t type_hash_of(const rosidl_message_type_support_t * type_support)
{
  rosidl_type_hash_t result = rosidl_get_zero_initialized_type_hash();
  if (type_support != nullptr && type_support->get_type_hash_func != nullptr) {
    const rosidl_type_hash_t * hash = type_support->get_type_hash_func(type_support);
    if (hash != nullptr) {result = *hash;}
  }
  return result;
}

template<typename DdsQos>
rmw_ret_t attach_type_hash(
  const rosidl_type_hash_t & type_hash, std::string & encoded, DdsQos & qos)
{
  const rosidl_type_hash_t * service_hash =
    rmw_zzdds_cpp::service_type_hash_override();
  rmw_ret_t result =
    rmw_dds_common::encode_type_hash_for_user_data_qos(type_hash, encoded);
  if (result != RMW_RET_OK) {return result;}
  if (service_hash != nullptr) {
    std::string encoded_service_hash;
    result = rmw_dds_common::encode_sertype_hash_for_user_data_qos(
      *service_hash, encoded_service_hash);
    if (result != RMW_RET_OK) {return result;}
    encoded += encoded_service_hash;
  }
  qos.user_data.value._maximum = static_cast<uint32_t>(encoded.size());
  qos.user_data.value._length = static_cast<uint32_t>(encoded.size());
  qos.user_data.value._buffer = encoded.empty() ? nullptr :
    reinterpret_cast<uint8_t *>(encoded.data());
  qos.user_data.value._release = false;
  return RMW_RET_OK;
}

bool fill_message_info(
  DDS_DataReader reader, const DDS_SampleInfo & source, rmw_message_info_t * destination)
{
  *destination = rmw_get_zero_initialized_message_info();
  destination->source_timestamp =
    RCUTILS_S_TO_NS(static_cast<rmw_time_point_value_t>(source.source_timestamp.sec)) +
    source.source_timestamp.nanosec;
  rcutils_time_point_value_t now = 0;
  if (rcutils_system_time_now(&now) == RCUTILS_RET_OK) {
    destination->received_timestamp = now;
  }
  destination->publication_sequence_number = RMW_MESSAGE_INFO_SEQUENCE_NUMBER_UNSUPPORTED;
  destination->reception_sequence_number = RMW_MESSAGE_INFO_SEQUENCE_NUMBER_UNSUPPORTED;
  if (!guid_from_publication(reader, source.publication_handle, &destination->publisher_gid)) {
    return false;
  }
  destination->from_intra_process = false;
  return true;
}

bool is_local_publication(SubscriptionImpl * subscription, DDS_InstanceHandle_t handle)
{
  rmw_gid_t gid{};
  if (!guid_from_publication(subscription->reader, handle, &gid)) {
    return false;
  }
  std::array<uint8_t, 16U> key{};
  std::memcpy(key.data(), gid.data, key.size());
  const std::lock_guard<std::mutex> lock(subscription->context->mutex);
  return subscription->context->local_publication_guids.count(key) != 0U;
}

DDS_ReturnCode_t take_visible_sample(
  SubscriptionImpl * subscription, RawLoan * loan, DDS_SampleInfo * info)
{
  while (true) {
    const DDS_ReturnCode_t result = DDS_DataReader_take_raw(
      subscription->reader, &loan->payloads, &loan->key_hashes, &loan->infos,
      DDS_HANDLE_NIL, nullptr, DDS_ANY_SAMPLE_STATE, DDS_ANY_VIEW_STATE,
      DDS_ANY_INSTANCE_STATE, 1);
    if (result == DDS_RETCODE_OK) {
      // The generated raw API reports an empty take as OK with empty
      // sequences, whereas the typed DDS take APIs report NO_DATA.
      if (loan->payloads._length == 0U && loan->infos._length == 0U) {
        const DDS_ReturnCode_t return_result = return_raw_loan(subscription->reader, loan);
        return return_result == DDS_RETCODE_OK ? DDS_RETCODE_NO_DATA : return_result;
      }
      if (loan->payloads._length != 1U || loan->payloads._buffer == nullptr ||
        loan->infos._length != 1U || loan->infos._buffer == nullptr)
      {
        (void)return_raw_loan(subscription->reader, loan);
        return DDS_RETCODE_ERROR;
      }
      loan->data = loan->payloads._buffer[0]._buffer;
      loan->data_len = loan->payloads._buffer[0]._length;
      *info = loan->infos._buffer[0];
    }
    if (result != DDS_RETCODE_OK || !subscription->ignore_local_publications ||
      !is_local_publication(subscription, info->publication_handle))
    {
      return result;
    }
    (void)return_raw_loan(subscription->reader, loan);
    *info = {};
  }
}

rmw_ret_t take_compatibility_loan(
  SubscriptionImpl * impl, void ** ros_message, bool * taken,
  rmw_message_info_t * message_info)
{
  void * message = impl->callbacks->create_ros_message();
  if (message == nullptr) {
    RMW_SET_ERROR_MSG("failed to allocate a loaned ROS message");
    return RMW_RET_BAD_ALLOC;
  }
  RawLoan loan{};
  DDS_SampleInfo info{};
  const DDS_ReturnCode_t result = take_visible_sample(impl, &loan, &info);
  if (result == DDS_RETCODE_NO_DATA) {
    impl->callbacks->destroy_ros_message(message);
    return RMW_RET_OK;
  }
  if (result != DDS_RETCODE_OK) {
    impl->callbacks->destroy_ros_message(message);
    RMW_SET_ERROR_MSG("zzdds failed to take a sample");
    return RMW_RET_ERROR;
  }
  bool decoded = !info.valid_data;
  if (info.valid_data) {
    ZidlCdrReader reader{};
    decoded = zidl_cdr_reader_init(&reader, loan.data, loan.data_len) == ZIDL_CDR_OK &&
      impl->callbacks->deserialize(&reader, message);
  }
  const DDS_ReturnCode_t return_result = return_raw_loan(impl->reader, &loan);
  if (return_result != DDS_RETCODE_OK) {
    impl->callbacks->destroy_ros_message(message);
    RMW_SET_ERROR_MSG("zzdds failed to return a raw sample loan");
    return RMW_RET_ERROR;
  }
  if (!decoded) {
    impl->callbacks->destroy_ros_message(message);
    RMW_SET_ERROR_MSG("failed to deserialize the loaned ROS message");
    return RMW_RET_ERROR;
  }
  if (!info.valid_data) {
    impl->callbacks->destroy_ros_message(message);
    return RMW_RET_OK;
  }
  try {
    const std::lock_guard<std::mutex> lock(impl->context->mutex);
    impl->loaned_messages.insert(message);
  } catch (const std::bad_alloc &) {
    impl->callbacks->destroy_ros_message(message);
    RMW_SET_ERROR_MSG("failed to record a loaned ROS message");
    return RMW_RET_BAD_ALLOC;
  }
  if (message_info != nullptr && !fill_message_info(impl->reader, info, message_info)) {
    {
      const std::lock_guard<std::mutex> lock(impl->context->mutex);
      impl->loaned_messages.erase(message);
    }
    impl->callbacks->destroy_ros_message(message);
    RMW_SET_ERROR_MSG("failed to resolve the publisher RTPS GUID");
    return RMW_RET_ERROR;
  }
  *ros_message = message;
  *taken = true;
  return RMW_RET_OK;
}

bool make_dds_string_seq(
  const rmw_subscription_content_filter_options_t & options,
  std::vector<char *> & pointers, DDS_StringSeq & sequence)
{
  if (options.filter_expression == nullptr ||
    (options.expression_parameters.size != 0U && options.expression_parameters.data == nullptr) ||
    options.expression_parameters.size > UINT32_MAX)
  {
    RMW_SET_ERROR_MSG("invalid content filter options");
    return false;
  }
  try {
    pointers.reserve(options.expression_parameters.size);
    for (size_t i = 0U; i < options.expression_parameters.size; ++i) {
      if (options.expression_parameters.data[i] == nullptr) {
        RMW_SET_ERROR_MSG("content filter expression parameter is null");
        return false;
      }
      pointers.push_back(options.expression_parameters.data[i]);
    }
  } catch (const std::bad_alloc &) {
    RMW_SET_ERROR_MSG("failed to allocate content filter parameter bookkeeping");
    return false;
  }
  sequence._maximum = static_cast<uint32_t>(pointers.size());
  sequence._length = static_cast<uint32_t>(pointers.size());
  sequence._buffer = pointers.empty() ? nullptr : pointers.data();
  sequence._release = false;
  return true;
}

DDS_ContentFilteredTopic create_filtered_topic(
  ContextImpl * context, DDS_Topic topic,
  const rmw_subscription_content_filter_options_t & options)
{
  std::vector<char *> pointers;
  DDS_StringSeq parameters{};
  if (!make_dds_string_seq(options, pointers, parameters)) {return nullptr;}
  std::array<char, 64> name{};
  const int length = std::snprintf(
    name.data(), name.size(), "rmw_zzdds_cft_%zu", context->next_content_filter_id++);
  if (length < 0 || static_cast<size_t>(length) >= name.size()) {
    RMW_SET_ERROR_MSG("failed to construct a unique content-filtered topic name");
    return nullptr;
  }
  DDS_ContentFilteredTopic result = DDS_DomainParticipant_create_contentfilteredtopic(
    context->dds.participant(), name.data(), topic, options.filter_expression, &parameters);
  if (result == nullptr) {
    RMW_SET_ERROR_MSG("zzdds failed to create the content-filtered topic");
  }
  return result;
}

rmw_ret_t replace_subscription_filter(
  SubscriptionImpl * subscription,
  const rmw_subscription_content_filter_options_t & options)
{
  DDS_ContentFilteredTopic new_filtered_topic = nullptr;
  DDS_TopicDescription description = zzdds_topic_as_description(subscription->topic);
  if (options.filter_expression[0] != '\0') {
    new_filtered_topic = create_filtered_topic(subscription->context, subscription->topic, options);
    if (new_filtered_topic == nullptr) {return RMW_RET_ERROR;}
    description = DDS_ContentFilteredTopic_as_DDS_TopicDescription(new_filtered_topic);
  }
  DDS_DataReaderQos dds_qos{};
  DDS_DataReaderQos_default(&dds_qos);
  if (!apply_qos(subscription->qos, dds_qos)) {
    DDS_DataReaderQos_free(&dds_qos);
    if (new_filtered_topic != nullptr) {
      (void)DDS_DomainParticipant_delete_contentfilteredtopic(
        subscription->context->dds.participant(), new_filtered_topic);
    }
    return RMW_RET_ERROR;
  }
  DDS_DataReader new_reader = DDS_Subscriber_create_datareader(
    subscription->context->dds.subscriber(), description, &dds_qos,
    nullptr, DDS_STATUS_MASK_NONE);
  DDS_DataReaderQos_free(&dds_qos);
  if (new_reader == nullptr) {
    if (new_filtered_topic != nullptr) {
      (void)DDS_DomainParticipant_delete_contentfilteredtopic(
        subscription->context->dds.participant(), new_filtered_topic);
    }
    RMW_SET_ERROR_MSG("zzdds failed to recreate the filtered data reader");
    return RMW_RET_ERROR;
  }
  DDS_ReadCondition new_condition = DDS_DataReader_create_readcondition(
    new_reader, DDS_NOT_READ_SAMPLE_STATE, DDS_ANY_VIEW_STATE, DDS_ANY_INSTANCE_STATE);
  if (new_condition == nullptr) {
    (void)DDS_Subscriber_delete_datareader(subscription->context->dds.subscriber(), new_reader);
    if (new_filtered_topic != nullptr) {
      (void)DDS_DomainParticipant_delete_contentfilteredtopic(
        subscription->context->dds.participant(), new_filtered_topic);
    }
    RMW_SET_ERROR_MSG("zzdds failed to recreate the subscription read condition");
    return RMW_RET_ERROR;
  }

  bool removed = DDS_DataReader_delete_readcondition(
    subscription->reader, subscription->read_condition) == DDS_RETCODE_OK;
  removed = DDS_Subscriber_delete_datareader(
    subscription->context->dds.subscriber(), subscription->reader) == DDS_RETCODE_OK && removed;
  if (subscription->filtered_topic != nullptr) {
    removed = DDS_DomainParticipant_delete_contentfilteredtopic(
      subscription->context->dds.participant(), subscription->filtered_topic) ==
      DDS_RETCODE_OK && removed;
  }
  subscription->filtered_topic = new_filtered_topic;
  subscription->reader = new_reader;
  subscription->read_condition = new_condition;
  if (!removed) {
    RMW_SET_ERROR_MSG("failed to destroy the previous filtered subscription resources");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

}  // namespace

extern "C"
{

rmw_publisher_t * rmw_create_publisher(
  const rmw_node_t * node, const rosidl_message_type_support_t * type_support,
  const char * topic_name, const rmw_qos_profile_t * qos,
  const rmw_publisher_options_t * options)
{
  if (!valid_node(node) || qos == nullptr || options == nullptr) {
    RMW_SET_ERROR_MSG("a valid node, QoS profile, and publisher options are required");
    return nullptr;
  }
  if (!validate_topic(topic_name, qos->avoid_ros_namespace_conventions)) {
    return nullptr;
  }
  if (options->require_unique_network_flow_endpoints ==
    RMW_UNIQUE_NETWORK_FLOW_ENDPOINTS_STRICTLY_REQUIRED)
  {
    RMW_SET_ERROR_MSG("unique network flow endpoints are not supported");
    return nullptr;
  }
  const auto * callbacks = rmw_zzdds_cpp::resolve_message_type_support(type_support);
  if (callbacks == nullptr) {
    return nullptr;
  }
  std::array<char, 264> dds_name{};
  if (!dds_topic_name(topic_name, qos->avoid_ros_namespace_conventions, dds_name)) {
    return nullptr;
  }
  auto * context = reinterpret_cast<ContextImpl *>(node->context->impl);
  const std::lock_guard<std::mutex> lock(context->mutex);
  if (context->shutdown) {
    RMW_SET_ERROR_MSG("cannot create a publisher after context shutdown");
    return nullptr;
  }
  if (zzdds_register_type_support(context->dds.participant(), callbacks->dds_type_name, nullptr,
      callbacks->get_field_from_cdr) != 0)
  {
    RMW_SET_ERROR_MSG("failed to register zzdds type support");
    return nullptr;
  }
  DDS_Topic topic = DDS_DomainParticipant_create_topic(
    context->dds.participant(), dds_name.data(), callbacks->dds_type_name,
    nullptr, nullptr, DDS_STATUS_MASK_NONE);
  if (topic == nullptr) {
    RMW_SET_ERROR_MSG("failed to create the DDS topic");
    return nullptr;
  }
  DDS_DataWriterQos dds_qos{};
  DDS_DataWriterQos_default(&dds_qos);
  if (!apply_qos(*qos, dds_qos)) {
    DDS_DataWriterQos_free(&dds_qos);
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    return nullptr;
  }
  const rosidl_type_hash_t type_hash = type_hash_of(type_support);
  std::string encoded_type_hash;
  if (attach_type_hash(type_hash, encoded_type_hash, dds_qos) != RMW_RET_OK) {
    DDS_DataWriterQos_free(&dds_qos);
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    RMW_SET_ERROR_MSG("failed to encode the publisher type hash");
    return nullptr;
  }
  const rmw_qos_profile_t actual_qos = resolved_qos(*qos, dds_qos);
  DDS_DataWriter writer = DDS_Publisher_create_datawriter(
    context->dds.publisher(), topic, &dds_qos, nullptr, DDS_STATUS_MASK_NONE);
  DDS_DataWriterQos_free(&dds_qos);
  if (writer == nullptr) {
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    RMW_SET_ERROR_MSG("failed to create the DDS data writer");
    return nullptr;
  }
  const auto allocator = context->allocator;
  auto * impl = rmw_zzdds_cpp::allocate_object<PublisherImpl>(
    allocator, context, node, topic, writer, callbacks, type_hash, actual_qos);
  auto * result = rmw_zzdds_cpp::allocate_object<rmw_publisher_t>(allocator);
  char * name_copy = rmw_zzdds_cpp::duplicate_string(allocator, topic_name);
  if (impl == nullptr || result == nullptr || name_copy == nullptr) {
    if (name_copy != nullptr) {allocator.deallocate(name_copy, allocator.state);}
    rmw_zzdds_cpp::deallocate_object(allocator, result);
    rmw_zzdds_cpp::deallocate_object(allocator, impl);
    (void)DDS_Publisher_delete_datawriter(context->dds.publisher(), writer);
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    RMW_SET_ERROR_MSG("failed to allocate publisher resources");
    return nullptr;
  }
  result->implementation_identifier = rmw_zzdds_cpp::identifier;
  result->data = impl;
  result->topic_name = name_copy;
  result->options = *options;
  result->can_loan_messages = true;
  rmw_gid_t publisher_gid{};
  if (!guid_from_writer(writer, &publisher_gid)) {
    allocator.deallocate(name_copy, allocator.state);
    rmw_zzdds_cpp::deallocate_object(allocator, result);
    rmw_zzdds_cpp::deallocate_object(allocator, impl);
    (void)DDS_Publisher_delete_datawriter(context->dds.publisher(), writer);
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    RMW_SET_ERROR_MSG("failed to obtain the zzdds writer RTPS GUID");
    return nullptr;
  }
  std::array<uint8_t, 16U> publication_guid{};
  std::memcpy(publication_guid.data(), publisher_gid.data, publication_guid.size());
  impl->rtps_guid = publication_guid;
  try {
    context->local_publication_guids.insert(publication_guid);
    context->publishers.insert(result);
    if (!context->suppress_graph_updates) {
      context->common.graph_cache.add_entity(
        publisher_gid, dds_name.data(), callbacks->dds_type_name, type_hash,
        context->participant_gid, actual_qos, false,
        rmw_zzdds_cpp::service_type_hash_override());
      if (context->common.add_publisher_graph(publisher_gid, node->name, node->namespace_) !=
        RMW_RET_OK)
      {
        context->common.graph_cache.remove_writer(publisher_gid);
        throw std::bad_alloc();
      }
      context->notify_graph_change();
    }
  } catch (const std::bad_alloc &) {
    context->local_publication_guids.erase(publication_guid);
    context->publishers.erase(result);
    allocator.deallocate(name_copy, allocator.state);
    rmw_zzdds_cpp::deallocate_object(allocator, result);
    rmw_zzdds_cpp::deallocate_object(allocator, impl);
    (void)DDS_Publisher_delete_datawriter(context->dds.publisher(), writer);
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    RMW_SET_ERROR_MSG("failed to record the local zzdds publication GUID");
    return nullptr;
  }
  ++context->active_entities;
  return result;
}

rmw_ret_t rmw_destroy_publisher(rmw_node_t * node, rmw_publisher_t * publisher)
{
  if (node == nullptr || publisher == nullptr || publisher->data == nullptr) {
    RMW_SET_ERROR_MSG("a valid node and publisher are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (node->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("node belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (!valid_node(node)) {
    RMW_SET_ERROR_MSG("a valid node and publisher are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (publisher->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("publisher belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  auto * impl = static_cast<PublisherImpl *>(publisher->data);
  auto * node_impl = static_cast<rmw_zzdds_cpp::NodeImpl *>(node->data);
  if (node_impl->context != node->context ||
    impl->context != reinterpret_cast<ContextImpl *>(node->context->impl))
  {
    RMW_SET_ERROR_MSG("publisher was not created by this node context");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * context = impl->context;
  {
    const std::lock_guard<std::mutex> lock(context->mutex);
    context->publishers.erase(publisher);
    context->local_publication_guids.erase(impl->rtps_guid);
    const rmw_gid_t publisher_gid = [&]() {
        rmw_gid_t value{};
        value.implementation_identifier = rmw_zzdds_cpp::identifier;
        std::memcpy(value.data, impl->rtps_guid.data(), impl->rtps_guid.size());
        return value;
      }();
    if (!context->suppress_graph_updates) {
      (void)context->common.remove_publisher_graph(
        publisher_gid, node->name, node->namespace_);
      context->common.graph_cache.remove_writer(publisher_gid);
      context->notify_graph_change();
    }
  }
  (void)DDS_DataWriter_set_listener(impl->writer, nullptr, DDS_STATUS_MASK_NONE);
  rmw_zzdds_cpp::cleanup_endpoint_events(impl);
  for (void * message : impl->loaned_messages) {
    impl->callbacks->destroy_ros_message(message);
  }
  impl->loaned_messages.clear();
  bool ok = DDS_Publisher_delete_datawriter(context->dds.publisher(),
      impl->writer) == DDS_RETCODE_OK;
  ok = (DDS_DomainParticipant_delete_topic(context->dds.participant(),
      impl->topic) == DDS_RETCODE_OK) && ok;
  const auto allocator = context->allocator;
  allocator.deallocate(const_cast<char *>(publisher->topic_name), allocator.state);
  rmw_zzdds_cpp::deallocate_object(allocator, impl);
  rmw_zzdds_cpp::deallocate_object(allocator, publisher);
  --context->active_entities;
  if (!ok) {RMW_SET_ERROR_MSG("failed to destroy one or more DDS publisher resources");}
  return ok ? RMW_RET_OK : RMW_RET_ERROR;
}

rmw_ret_t rmw_publish(
  const rmw_publisher_t * publisher, const void * ros_message, rmw_publisher_allocation_t *)
{
  const rmw_ret_t validation = validate_publisher(publisher);
  if (validation != RMW_RET_OK) {return validation;}
  if (ros_message == nullptr) {
    RMW_SET_ERROR_MSG("a valid publisher and ROS message are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<PublisherImpl *>(publisher->data);
  ZidlCdrWriter writer{};
  if (zidl_cdr_writer_init(&writer, ZIDL_XCDR1) != ZIDL_CDR_OK) {
    RMW_SET_ERROR_MSG("failed to initialize the CDR writer");
    return RMW_RET_BAD_ALLOC;
  }
  const bool serialized = impl->callbacks->serialize(ros_message, &writer);
  const rmw_ret_t result = serialized ?
    write_serialized(impl, writer.buf, writer.len, ros_message) : RMW_RET_ERROR;
  zidl_cdr_writer_deinit(&writer);
  if (!serialized) {RMW_SET_ERROR_MSG("failed to serialize the ROS message");}
  return result;
}

rmw_ret_t rmw_borrow_loaned_message(
  const rmw_publisher_t * publisher,
  const rosidl_message_type_support_t * type_support, void ** ros_message)
{
  const rmw_ret_t validation = validate_publisher(publisher);
  if (validation != RMW_RET_OK) {return validation;}
  if (type_support == nullptr || ros_message == nullptr || *ros_message != nullptr) {
    RMW_SET_ERROR_MSG("valid type support and an empty loaned-message output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<PublisherImpl *>(publisher->data);
  const auto * callbacks = rmw_zzdds_cpp::resolve_message_type_support(type_support);
  if (callbacks == nullptr) {return RMW_RET_INVALID_ARGUMENT;}
  if (callbacks != impl->callbacks) {
    RMW_SET_ERROR_MSG("loaned-message type support does not match the publisher");
    return RMW_RET_INVALID_ARGUMENT;
  }
  void * message = callbacks->create_ros_message();
  if (message == nullptr) {
    RMW_SET_ERROR_MSG("failed to allocate a loaned ROS message");
    return RMW_RET_BAD_ALLOC;
  }
  try {
    const std::lock_guard<std::mutex> lock(impl->context->mutex);
    impl->loaned_messages.insert(message);
  } catch (const std::bad_alloc &) {
    callbacks->destroy_ros_message(message);
    RMW_SET_ERROR_MSG("failed to record a loaned ROS message");
    return RMW_RET_BAD_ALLOC;
  }
  *ros_message = message;
  return RMW_RET_OK;
}

rmw_ret_t rmw_return_loaned_message_from_publisher(
  const rmw_publisher_t * publisher, void * loaned_message)
{
  const rmw_ret_t validation = validate_publisher(publisher);
  if (validation != RMW_RET_OK) {return validation;}
  if (loaned_message == nullptr) {
    RMW_SET_ERROR_MSG("a loaned ROS message is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<PublisherImpl *>(publisher->data);
  {
    const std::lock_guard<std::mutex> lock(impl->context->mutex);
    if (impl->loaned_messages.erase(loaned_message) == 0U) {
      RMW_SET_ERROR_MSG("message was not borrowed from this publisher");
      return RMW_RET_ERROR;
    }
  }
  impl->callbacks->destroy_ros_message(loaned_message);
  return RMW_RET_OK;
}

rmw_ret_t rmw_publish_loaned_message(
  const rmw_publisher_t * publisher, void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  const rmw_ret_t validation = validate_publisher(publisher);
  if (validation != RMW_RET_OK) {return validation;}
  if (ros_message == nullptr) {
    RMW_SET_ERROR_MSG("a loaned ROS message is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<PublisherImpl *>(publisher->data);
  {
    const std::lock_guard<std::mutex> lock(impl->context->mutex);
    if (impl->loaned_messages.erase(ros_message) == 0U) {
      RMW_SET_ERROR_MSG("message was not borrowed from this publisher");
      return RMW_RET_ERROR;
    }
  }
  const rmw_ret_t result = rmw_publish(publisher, ros_message, allocation);
  impl->callbacks->destroy_ros_message(ros_message);
  return result;
}

rmw_ret_t rmw_publish_serialized_message(
  const rmw_publisher_t * publisher, const rmw_serialized_message_t * message,
  rmw_publisher_allocation_t *)
{
  const rmw_ret_t validation = validate_publisher(publisher);
  if (validation != RMW_RET_OK) {return validation;}
  if (message == nullptr || message->buffer == nullptr ||
    message->buffer_length < 4U)
  {
    RMW_SET_ERROR_MSG("a valid publisher and serialized message are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<PublisherImpl *>(publisher->data);
  if (impl->callbacks->has_key) {
    RMW_SET_ERROR_MSG("serialized publishing of keyed messages is not yet supported");
    return RMW_RET_UNSUPPORTED;
  }
  return write_serialized(impl, message->buffer, message->buffer_length, nullptr);
}

rmw_ret_t rmw_publisher_get_actual_qos(
  const rmw_publisher_t * publisher, rmw_qos_profile_t * qos)
{
  const rmw_ret_t validation = validate_publisher(publisher);
  if (validation != RMW_RET_OK) {return validation;}
  if (qos == nullptr) {
    RMW_SET_ERROR_MSG("a valid publisher and output QoS profile are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  *qos = static_cast<PublisherImpl *>(publisher->data)->qos;
  return RMW_RET_OK;
}

rmw_ret_t rmw_publisher_count_matched_subscriptions(
  const rmw_publisher_t * publisher, size_t * count)
{
  const rmw_ret_t validation = validate_publisher(publisher);
  if (validation != RMW_RET_OK) {return validation;}
  if (count == nullptr) {
    RMW_SET_ERROR_MSG("a valid publisher and count output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  DDS_PublicationMatchedStatus status{};
  if (DDS_DataWriter_get_publication_matched_status(
      static_cast<PublisherImpl *>(publisher->data)->writer, &status) != DDS_RETCODE_OK)
  {
    RMW_SET_ERROR_MSG("failed to query matched subscriptions");
    return RMW_RET_ERROR;
  }
  *count = status.current_count < 0 ? 0U : static_cast<size_t>(status.current_count);
  return RMW_RET_OK;
}

rmw_ret_t rmw_publisher_wait_for_all_acked(
  const rmw_publisher_t * publisher, rmw_time_t wait_timeout)
{
  const rmw_ret_t validation = validate_publisher(publisher);
  if (validation != RMW_RET_OK) {return validation;}
  auto * impl = static_cast<PublisherImpl *>(publisher->data);
  if (impl->qos.reliability == RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT) {
    return RMW_RET_OK;
  }
  constexpr uint64_t billion = 1000000000ULL;
  DDS_Duration_t duration{};
  if (wait_timeout.sec == 9223372036ULL && wait_timeout.nsec == 854775807ULL) {
    duration.sec = DDS_DURATION_INFINITE_SEC;
    duration.nanosec = DDS_DURATION_INFINITE_NSEC;
  } else {
    const uint64_t extra_seconds = wait_timeout.nsec / billion;
    if (wait_timeout.sec > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
      extra_seconds > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) -
      wait_timeout.sec)
    {
      RMW_SET_ERROR_MSG("acknowledgment timeout exceeds the DDS duration range");
      return RMW_RET_INVALID_ARGUMENT;
    }
    duration.sec = static_cast<int32_t>(wait_timeout.sec + extra_seconds);
    duration.nanosec = static_cast<uint32_t>(wait_timeout.nsec % billion);
  }
  const DDS_ReturnCode_t result = DDS_DataWriter_wait_for_acknowledgments(
    impl->writer, &duration);
  if (result == DDS_RETCODE_TIMEOUT) {return RMW_RET_TIMEOUT;}
  if (result != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("zzdds failed while waiting for acknowledgments");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

rmw_subscription_t * rmw_create_subscription(
  const rmw_node_t * node, const rosidl_message_type_support_t * type_support,
  const char * topic_name, const rmw_qos_profile_t * qos,
  const rmw_subscription_options_t * options)
{
  if (!valid_node(node) || qos == nullptr || options == nullptr) {
    RMW_SET_ERROR_MSG("a valid node, QoS profile, and subscription options are required");
    return nullptr;
  }
  if (options->content_filter_options != nullptr &&
    options->content_filter_options->filter_expression == nullptr)
  {
    RMW_SET_ERROR_MSG("content filter expression must not be null");
    return nullptr;
  }
  if (!validate_topic(topic_name, qos->avoid_ros_namespace_conventions)) {return nullptr;}
  if (options->require_unique_network_flow_endpoints ==
    RMW_UNIQUE_NETWORK_FLOW_ENDPOINTS_STRICTLY_REQUIRED)
  {
    RMW_SET_ERROR_MSG("requested subscription options are not supported");
    return nullptr;
  }
  const auto * callbacks = rmw_zzdds_cpp::resolve_message_type_support(type_support);
  if (callbacks == nullptr) {return nullptr;}
  std::array<char, 264> dds_name{};
  if (!dds_topic_name(topic_name, qos->avoid_ros_namespace_conventions, dds_name)) {return nullptr;}
  auto * context = reinterpret_cast<ContextImpl *>(node->context->impl);
  const std::lock_guard<std::mutex> lock(context->mutex);
  if (context->shutdown) {
    RMW_SET_ERROR_MSG("cannot create a subscription after context shutdown");
    return nullptr;
  }
  if (zzdds_register_type_support(context->dds.participant(), callbacks->dds_type_name, nullptr,
      callbacks->get_field_from_cdr) != 0)
  {
    RMW_SET_ERROR_MSG("failed to register zzdds type support");
    return nullptr;
  }
  DDS_Topic topic = DDS_DomainParticipant_create_topic(
    context->dds.participant(), dds_name.data(), callbacks->dds_type_name,
    nullptr, nullptr, DDS_STATUS_MASK_NONE);
  if (topic == nullptr) {RMW_SET_ERROR_MSG("failed to create the DDS topic"); return nullptr;}
  DDS_DataReaderQos dds_qos{};
  DDS_DataReaderQos_default(&dds_qos);
  if (!apply_qos(*qos, dds_qos)) {
    DDS_DataReaderQos_free(&dds_qos);
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    return nullptr;
  }
  const rosidl_type_hash_t type_hash = type_hash_of(type_support);
  std::string encoded_type_hash;
  if (attach_type_hash(type_hash, encoded_type_hash, dds_qos) != RMW_RET_OK) {
    DDS_DataReaderQos_free(&dds_qos);
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    RMW_SET_ERROR_MSG("failed to encode the subscription type hash");
    return nullptr;
  }
  const rmw_qos_profile_t actual_qos = resolved_qos(*qos, dds_qos);
  DDS_ContentFilteredTopic filtered_topic = nullptr;
  DDS_TopicDescription topic_description = zzdds_topic_as_description(topic);
  if (options->content_filter_options != nullptr &&
    options->content_filter_options->filter_expression != nullptr &&
    options->content_filter_options->filter_expression[0] != '\0')
  {
    filtered_topic = create_filtered_topic(context, topic, *options->content_filter_options);
    if (filtered_topic == nullptr) {
      DDS_DataReaderQos_free(&dds_qos);
      (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
      return nullptr;
    }
    topic_description = DDS_ContentFilteredTopic_as_DDS_TopicDescription(filtered_topic);
  }
  DDS_DataReader reader = DDS_Subscriber_create_datareader(
    context->dds.subscriber(), topic_description, &dds_qos,
    nullptr, DDS_STATUS_MASK_NONE);
  DDS_DataReaderQos_free(&dds_qos);
  if (reader == nullptr) {
    if (filtered_topic != nullptr) {
      (void)DDS_DomainParticipant_delete_contentfilteredtopic(
        context->dds.participant(), filtered_topic);
    }
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    RMW_SET_ERROR_MSG("failed to create the DDS data reader");
    return nullptr;
  }
  DDS_ReadCondition read_condition = DDS_DataReader_create_readcondition(
    reader, DDS_NOT_READ_SAMPLE_STATE, DDS_ANY_VIEW_STATE, DDS_ANY_INSTANCE_STATE);
  if (read_condition == nullptr) {
    (void)DDS_Subscriber_delete_datareader(context->dds.subscriber(), reader);
    if (filtered_topic != nullptr) {
      (void)DDS_DomainParticipant_delete_contentfilteredtopic(
        context->dds.participant(), filtered_topic);
    }
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    RMW_SET_ERROR_MSG("failed to create the DDS subscription read condition");
    return nullptr;
  }
  const auto allocator = context->allocator;
  auto * impl = rmw_zzdds_cpp::allocate_object<SubscriptionImpl>(
    allocator, context, node, topic, filtered_topic, reader, read_condition, callbacks, actual_qos,
    type_hash, options->ignore_local_publications);
  auto * result = rmw_zzdds_cpp::allocate_object<rmw_subscription_t>(allocator);
  char * name_copy = rmw_zzdds_cpp::duplicate_string(allocator, topic_name);
  if (impl == nullptr || result == nullptr || name_copy == nullptr) {
    if (name_copy != nullptr) {allocator.deallocate(name_copy, allocator.state);}
    rmw_zzdds_cpp::deallocate_object(allocator, result);
    rmw_zzdds_cpp::deallocate_object(allocator, impl);
    (void)DDS_DataReader_delete_readcondition(reader, read_condition);
    (void)DDS_Subscriber_delete_datareader(context->dds.subscriber(), reader);
    if (filtered_topic != nullptr) {
      (void)DDS_DomainParticipant_delete_contentfilteredtopic(
        context->dds.participant(), filtered_topic);
    }
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    RMW_SET_ERROR_MSG("failed to allocate subscription resources");
    return nullptr;
  }
  result->implementation_identifier = rmw_zzdds_cpp::identifier;
  result->data = impl;
  result->topic_name = name_copy;
  result->options = *options;
  result->can_loan_messages = true;
  result->is_cft_enabled = filtered_topic != nullptr;
  result->is_cft_supported = true;
  rmw_gid_t subscription_gid{};
  if (!guid_from_reader(reader, &subscription_gid)) {
    allocator.deallocate(name_copy, allocator.state);
    rmw_zzdds_cpp::deallocate_object(allocator, result);
    rmw_zzdds_cpp::deallocate_object(allocator, impl);
    (void)DDS_DataReader_delete_readcondition(reader, read_condition);
    (void)DDS_Subscriber_delete_datareader(context->dds.subscriber(), reader);
    if (filtered_topic != nullptr) {
      (void)DDS_DomainParticipant_delete_contentfilteredtopic(
        context->dds.participant(), filtered_topic);
    }
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    RMW_SET_ERROR_MSG("failed to obtain the zzdds reader RTPS GUID");
    return nullptr;
  }
  try {
    context->subscriptions.insert(result);
    if (!context->suppress_graph_updates) {
      context->common.graph_cache.add_entity(
        subscription_gid, dds_name.data(), callbacks->dds_type_name, type_hash,
        context->participant_gid, actual_qos, true,
        rmw_zzdds_cpp::service_type_hash_override());
      if (context->common.add_subscriber_graph(subscription_gid, node->name, node->namespace_) !=
        RMW_RET_OK)
      {
        context->common.graph_cache.remove_reader(subscription_gid);
        throw std::bad_alloc();
      }
      context->notify_graph_change();
    }
  } catch (const std::bad_alloc &) {
    context->subscriptions.erase(result);
    allocator.deallocate(name_copy, allocator.state);
    rmw_zzdds_cpp::deallocate_object(allocator, result);
    rmw_zzdds_cpp::deallocate_object(allocator, impl);
    (void)DDS_DataReader_delete_readcondition(reader, read_condition);
    (void)DDS_Subscriber_delete_datareader(context->dds.subscriber(), reader);
    if (filtered_topic != nullptr) {
      (void)DDS_DomainParticipant_delete_contentfilteredtopic(
        context->dds.participant(), filtered_topic);
    }
    (void)DDS_DomainParticipant_delete_topic(context->dds.participant(), topic);
    RMW_SET_ERROR_MSG("failed to register the subscription in the local graph");
    return nullptr;
  }
  ++context->active_entities;
  return result;
}

rmw_ret_t rmw_destroy_subscription(rmw_node_t * node, rmw_subscription_t * subscription)
{
  if (node == nullptr || subscription == nullptr || subscription->data == nullptr) {
    RMW_SET_ERROR_MSG("a valid node and subscription are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (node->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("node belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (!valid_node(node)) {
    RMW_SET_ERROR_MSG("a valid node and subscription are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (subscription->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("subscription belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  auto * impl = static_cast<SubscriptionImpl *>(subscription->data);
  if (impl->context != reinterpret_cast<ContextImpl *>(node->context->impl)) {
    RMW_SET_ERROR_MSG("subscription was not created by this node context");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * context = impl->context;
  rmw_gid_t subscription_gid{};
  const bool have_subscription_gid = guid_from_reader(impl->reader, &subscription_gid);
  {
    const std::lock_guard<std::mutex> lock(context->mutex);
    context->subscriptions.erase(subscription);
    if (have_subscription_gid && !context->suppress_graph_updates) {
      (void)context->common.remove_subscriber_graph(
        subscription_gid, node->name, node->namespace_);
      context->common.graph_cache.remove_reader(subscription_gid);
      context->notify_graph_change();
    }
  }
  (void)DDS_DataReader_set_listener(impl->reader, nullptr, DDS_STATUS_MASK_NONE);
  rmw_zzdds_cpp::cleanup_endpoint_events(impl);
  for (void * message : impl->loaned_messages) {
    impl->callbacks->destroy_ros_message(message);
  }
  impl->loaned_messages.clear();
  bool ok = DDS_DataReader_delete_readcondition(impl->reader,
      impl->read_condition) == DDS_RETCODE_OK;
  ok = (DDS_Subscriber_delete_datareader(context->dds.subscriber(),
      impl->reader) == DDS_RETCODE_OK) && ok;
  if (impl->filtered_topic != nullptr) {
    ok = (DDS_DomainParticipant_delete_contentfilteredtopic(
        context->dds.participant(), impl->filtered_topic) == DDS_RETCODE_OK) && ok;
  }
  ok = (DDS_DomainParticipant_delete_topic(context->dds.participant(),
      impl->topic) == DDS_RETCODE_OK) && ok;
  const auto allocator = context->allocator;
  allocator.deallocate(const_cast<char *>(subscription->topic_name), allocator.state);
  rmw_zzdds_cpp::deallocate_object(allocator, impl);
  rmw_zzdds_cpp::deallocate_object(allocator, subscription);
  --context->active_entities;
  if (!ok) {RMW_SET_ERROR_MSG("failed to destroy one or more DDS subscription resources");}
  return ok ? RMW_RET_OK : RMW_RET_ERROR;
}

rmw_ret_t rmw_take(
  const rmw_subscription_t * subscription, void * ros_message, bool * taken,
  rmw_subscription_allocation_t *)
{
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (ros_message == nullptr || taken == nullptr) {
    RMW_SET_ERROR_MSG("a valid subscription, ROS message, and taken flag are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  *taken = false;
  auto * impl = static_cast<SubscriptionImpl *>(subscription->data);
  RawLoan loan{};
  DDS_SampleInfo info{};
  const DDS_ReturnCode_t result = take_visible_sample(impl, &loan, &info);
  if (result == DDS_RETCODE_NO_DATA) {return RMW_RET_OK;}
  if (result != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("zzdds failed to take a sample"); return RMW_RET_ERROR;
  }
  bool decoded = false;
  if (info.valid_data) {
    ZidlCdrReader reader{};
    decoded = zidl_cdr_reader_init(&reader, loan.data, loan.data_len) == ZIDL_CDR_OK &&
      impl->callbacks->deserialize(&reader, ros_message);
  }
  const DDS_ReturnCode_t return_result = return_raw_loan(impl->reader, &loan);
  if (return_result != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("zzdds failed to return a raw sample loan");
    return RMW_RET_ERROR;
  }
  if (info.valid_data && !decoded) {
    RMW_SET_ERROR_MSG("failed to deserialize the taken sample"); return RMW_RET_ERROR;
  }
  *taken = info.valid_data;
  return RMW_RET_OK;
}

rmw_ret_t rmw_take_loaned_message(
  const rmw_subscription_t * subscription, void ** loaned_message, bool * taken,
  rmw_subscription_allocation_t *)
{
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (loaned_message == nullptr || *loaned_message != nullptr || taken == nullptr) {
    RMW_SET_ERROR_MSG("an empty loaned-message output and taken flag are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  *taken = false;
  return take_compatibility_loan(
    static_cast<SubscriptionImpl *>(subscription->data), loaned_message, taken, nullptr);
}

rmw_ret_t rmw_take_loaned_message_with_info(
  const rmw_subscription_t * subscription, void ** loaned_message, bool * taken,
  rmw_message_info_t * message_info, rmw_subscription_allocation_t *)
{
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (loaned_message == nullptr || *loaned_message != nullptr || taken == nullptr ||
    message_info == nullptr)
  {
    RMW_SET_ERROR_MSG("an empty loaned-message output, taken flag, and message info are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  *taken = false;
  return take_compatibility_loan(
    static_cast<SubscriptionImpl *>(subscription->data), loaned_message, taken, message_info);
}

rmw_ret_t rmw_return_loaned_message_from_subscription(
  const rmw_subscription_t * subscription, void * loaned_message)
{
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (loaned_message == nullptr) {
    RMW_SET_ERROR_MSG("a loaned ROS message is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<SubscriptionImpl *>(subscription->data);
  {
    const std::lock_guard<std::mutex> lock(impl->context->mutex);
    if (impl->loaned_messages.erase(loaned_message) == 0U) {
      RMW_SET_ERROR_MSG("message was not loaned by this subscription");
      return RMW_RET_ERROR;
    }
  }
  impl->callbacks->destroy_ros_message(loaned_message);
  return RMW_RET_OK;
}

rmw_ret_t rmw_take_serialized_message(
  const rmw_subscription_t * subscription, rmw_serialized_message_t * message,
  bool * taken, rmw_subscription_allocation_t *)
{
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (message == nullptr || taken == nullptr) {
    RMW_SET_ERROR_MSG("a valid subscription, serialized message, and taken flag are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  *taken = false;
  auto * impl = static_cast<SubscriptionImpl *>(subscription->data);
  RawLoan loan{};
  DDS_SampleInfo info{};
  const DDS_ReturnCode_t result = take_visible_sample(impl, &loan, &info);
  if (result == DDS_RETCODE_NO_DATA) {return RMW_RET_OK;}
  if (result != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("zzdds failed to take a sample"); return RMW_RET_ERROR;
  }
  if (info.valid_data && message->buffer_capacity < loan.data_len) {
    const rcutils_ret_t resize = rcutils_uint8_array_resize(message, loan.data_len);
    if (resize != RCUTILS_RET_OK) {
      (void)return_raw_loan(impl->reader, &loan);
      return resize == RCUTILS_RET_BAD_ALLOC ? RMW_RET_BAD_ALLOC : RMW_RET_ERROR;
    }
  }
  if (info.valid_data) {
    std::memcpy(message->buffer, loan.data, loan.data_len);
    message->buffer_length = loan.data_len;
  }
  (void)return_raw_loan(impl->reader, &loan);
  *taken = info.valid_data;
  return RMW_RET_OK;
}

rmw_ret_t rmw_take_with_info(
  const rmw_subscription_t * subscription, void * ros_message, bool * taken,
  rmw_message_info_t * message_info, rmw_subscription_allocation_t *)
{
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (ros_message == nullptr || taken == nullptr || message_info == nullptr) {
    RMW_SET_ERROR_MSG("a ROS message, taken flag, and message info are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  *taken = false;
  auto * impl = static_cast<SubscriptionImpl *>(subscription->data);
  RawLoan loan{};
  DDS_SampleInfo info{};
  const DDS_ReturnCode_t result = take_visible_sample(impl, &loan, &info);
  if (result == DDS_RETCODE_NO_DATA) {return RMW_RET_OK;}
  if (result != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("zzdds failed to take a sample");
    return RMW_RET_ERROR;
  }
  const bool decoded = !info.valid_data ||
    ([&]() {
      ZidlCdrReader reader{};
      return zidl_cdr_reader_init(&reader, loan.data, loan.data_len) == ZIDL_CDR_OK &&
             impl->callbacks->deserialize(&reader, ros_message);
    })();
  (void)return_raw_loan(impl->reader, &loan);
  if (!decoded) {
    RMW_SET_ERROR_MSG("failed to deserialize the taken sample");
    return RMW_RET_ERROR;
  }
  if (info.valid_data && !fill_message_info(impl->reader, info, message_info)) {
    RMW_SET_ERROR_MSG("failed to resolve the publisher RTPS GUID");
    return RMW_RET_ERROR;
  }
  *taken = info.valid_data;
  return RMW_RET_OK;
}

rmw_ret_t rmw_take_serialized_message_with_info(
  const rmw_subscription_t * subscription, rmw_serialized_message_t * message,
  bool * taken, rmw_message_info_t * message_info, rmw_subscription_allocation_t * allocation)
{
  if (message_info == nullptr) {
    RMW_SET_ERROR_MSG("message info is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (message == nullptr || taken == nullptr) {
    RMW_SET_ERROR_MSG("a serialized message and taken flag are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<SubscriptionImpl *>(subscription->data);
  RawLoan loan{};
  DDS_SampleInfo info{};
  *taken = false;
  const DDS_ReturnCode_t result = take_visible_sample(impl, &loan, &info);
  if (result == DDS_RETCODE_NO_DATA) {return RMW_RET_OK;}
  if (result != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("zzdds failed to take a sample");
    return RMW_RET_ERROR;
  }
  if (info.valid_data && message->buffer_capacity < loan.data_len &&
    rcutils_uint8_array_resize(message, loan.data_len) != RCUTILS_RET_OK)
  {
    (void)return_raw_loan(impl->reader, &loan);
    return RMW_RET_BAD_ALLOC;
  }
  if (info.valid_data) {
    std::memcpy(message->buffer, loan.data, loan.data_len);
    message->buffer_length = loan.data_len;
    if (!fill_message_info(impl->reader, info, message_info)) {
      (void)return_raw_loan(impl->reader, &loan);
      RMW_SET_ERROR_MSG("failed to resolve the publisher RTPS GUID");
      return RMW_RET_ERROR;
    }
  }
  (void)return_raw_loan(impl->reader, &loan);
  *taken = info.valid_data;
  (void)allocation;
  return RMW_RET_OK;
}

rmw_ret_t rmw_take_sequence(
  const rmw_subscription_t * subscription, size_t count,
  rmw_message_sequence_t * message_sequence,
  rmw_message_info_sequence_t * message_info_sequence, size_t * taken,
  rmw_subscription_allocation_t * allocation)
{
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (count == 0U || message_sequence == nullptr || message_info_sequence == nullptr ||
    taken == nullptr || message_sequence->data == nullptr || message_info_sequence->data == nullptr ||
    message_sequence->capacity < count || message_info_sequence->capacity < count)
  {
    RMW_SET_ERROR_MSG("valid sequences with capacity for count samples are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  *taken = 0U;
  for (size_t index = 0U; index < count; ++index) {
    bool one_taken = false;
    const rmw_ret_t result = rmw_take_with_info(
      subscription, message_sequence->data[index], &one_taken,
      &message_info_sequence->data[index], allocation);
    if (result != RMW_RET_OK) {return result;}
    if (!one_taken) {break;}
    ++*taken;
  }
  message_sequence->size = *taken;
  message_info_sequence->size = *taken;
  return RMW_RET_OK;
}

rmw_ret_t rmw_get_gid_for_publisher(const rmw_publisher_t * publisher, rmw_gid_t * gid)
{
  const rmw_ret_t validation = validate_publisher(publisher);
  if (validation != RMW_RET_OK) {return validation;}
  if (gid == nullptr) {
    RMW_SET_ERROR_MSG("a GID output is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  const auto * impl = static_cast<const PublisherImpl *>(publisher->data);
  if (!guid_from_writer(impl->writer, gid)) {
    RMW_SET_ERROR_MSG("failed to obtain the zzdds writer RTPS GUID");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

rmw_ret_t rmw_compare_gids_equal(
  const rmw_gid_t * gid1, const rmw_gid_t * gid2, bool * result)
{
  if (gid1 == nullptr || gid2 == nullptr || result == nullptr) {
    RMW_SET_ERROR_MSG("two GIDs and a result output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (gid1->implementation_identifier != rmw_zzdds_cpp::identifier ||
    gid2->implementation_identifier != rmw_zzdds_cpp::identifier)
  {
    RMW_SET_ERROR_MSG("GID belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  *result = std::memcmp(gid1->data, gid2->data, RMW_GID_STORAGE_SIZE) == 0;
  return RMW_RET_OK;
}

rmw_ret_t rmw_subscription_get_actual_qos(
  const rmw_subscription_t * subscription, rmw_qos_profile_t * qos)
{
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (qos == nullptr) {
    RMW_SET_ERROR_MSG("a valid subscription and output QoS profile are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  *qos = static_cast<SubscriptionImpl *>(subscription->data)->qos;
  return RMW_RET_OK;
}

rmw_ret_t rmw_subscription_set_content_filter(
  rmw_subscription_t * subscription,
  const rmw_subscription_content_filter_options_t * options)
{
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (options == nullptr || options->filter_expression == nullptr) {
    RMW_SET_ERROR_MSG("valid content filter options are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<SubscriptionImpl *>(subscription->data);
  const std::lock_guard<std::mutex> lock(impl->context->mutex);
  if (impl->filtered_topic != nullptr && options->filter_expression[0] != '\0' &&
    std::strcmp(
      DDS_ContentFilteredTopic_get_filter_expression(impl->filtered_topic),
      options->filter_expression) == 0)
  {
    std::vector<char *> pointers;
    DDS_StringSeq parameters{};
    if (!make_dds_string_seq(*options, pointers, parameters)) {
      return RMW_RET_INVALID_ARGUMENT;
    }
    if (DDS_ContentFilteredTopic_set_expression_parameters(
        impl->filtered_topic, &parameters) != DDS_RETCODE_OK)
    {
      RMW_SET_ERROR_MSG("zzdds failed to update content filter parameters");
      return RMW_RET_ERROR;
    }
    return RMW_RET_OK;
  }
  const rmw_ret_t result = replace_subscription_filter(impl, *options);
  if (result == RMW_RET_OK) {
    subscription->is_cft_enabled = impl->filtered_topic != nullptr;
  }
  return result;
}

rmw_ret_t rmw_subscription_get_content_filter(
  const rmw_subscription_t * subscription, rcutils_allocator_t * allocator,
  rmw_subscription_content_filter_options_t * options)
{
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (allocator == nullptr || options == nullptr || !rcutils_allocator_is_valid(allocator)) {
    RMW_SET_ERROR_MSG("a valid allocator and content filter output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  const auto * impl = static_cast<const SubscriptionImpl *>(subscription->data);
  if (impl->filtered_topic == nullptr) {
    RMW_SET_ERROR_MSG("subscription does not have an enabled content filter");
    return RMW_RET_UNSUPPORTED;
  }
  DDS_StringSeq parameters{};
  if (DDS_ContentFilteredTopic_get_expression_parameters(
      impl->filtered_topic, &parameters) != DDS_RETCODE_OK)
  {
    RMW_SET_ERROR_MSG("zzdds failed to retrieve content filter parameters");
    return RMW_RET_ERROR;
  }
  std::vector<const char *> pointers;
  try {
    pointers.reserve(parameters._length);
    for (uint32_t i = 0U; i < parameters._length; ++i) {
      pointers.push_back(parameters._buffer[i]);
    }
  } catch (const std::bad_alloc &) {
    DDS_StringSeq_free(&parameters);
    return RMW_RET_BAD_ALLOC;
  }
  const rmw_ret_t result = rmw_subscription_content_filter_options_init(
    DDS_ContentFilteredTopic_get_filter_expression(impl->filtered_topic),
    pointers.size(), pointers.empty() ? nullptr : pointers.data(), allocator, options);
  DDS_StringSeq_free(&parameters);
  return result;
}

rmw_ret_t rmw_subscription_count_matched_publishers(
  const rmw_subscription_t * subscription, size_t * count)
{
  const rmw_ret_t validation = validate_subscription(subscription);
  if (validation != RMW_RET_OK) {return validation;}
  if (count == nullptr) {
    RMW_SET_ERROR_MSG("a valid subscription and count output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  DDS_SubscriptionMatchedStatus status{};
  if (DDS_DataReader_get_subscription_matched_status(
      static_cast<SubscriptionImpl *>(subscription->data)->reader, &status) != DDS_RETCODE_OK)
  {
    RMW_SET_ERROR_MSG("failed to query matched publishers");
    return RMW_RET_ERROR;
  }
  *count = status.current_count < 0 ? 0U : static_cast<size_t>(status.current_count);
  return RMW_RET_OK;
}

}  // extern "C"
