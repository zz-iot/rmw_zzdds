#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>

#include "rmw/error_handling.h"
#include "rmw/rmw.h"
#include "rmw/serialized_message.h"

#include "rmw_zzdds_cpp/allocator.hpp"
#include "rmw_zzdds_cpp/endpoint_impl.hpp"
#include "rmw_zzdds_cpp/identifier.hpp"
#include "rmw_zzdds_cpp/node_impl.hpp"
#include "rmw_zzdds_cpp/service_impl.hpp"
#include "rmw_zzdds_cpp/service_type_hash.hpp"
#include "rosidl_typesupport_zzdds_cpp/identifier.hpp"

namespace
{
using rmw_zzdds_cpp::ClientImpl;
using rmw_zzdds_cpp::ContextImpl;
using rmw_zzdds_cpp::PublisherImpl;
using rmw_zzdds_cpp::ServiceImpl;
using rmw_zzdds_cpp::ServiceTypeSupport;
using rmw_zzdds_cpp::SubscriptionImpl;

constexpr size_t dds_guid_size = 16U;
constexpr size_t request_header_size = dds_guid_size + sizeof(int64_t);

rosidl_message_type_support_t message_handle(
  const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t * callbacks);

bool valid_node(const rmw_node_t * node)
{
  return node != nullptr && node->implementation_identifier == rmw_zzdds_cpp::identifier &&
         node->data != nullptr && node->context != nullptr && node->context->impl != nullptr;
}

template<typename Handle>
bool valid_handle(const Handle * handle)
{
  return handle != nullptr &&
         handle->implementation_identifier == rmw_zzdds_cpp::identifier &&
         handle->data != nullptr;
}

void encode_sequence(uint8_t * output, int64_t sequence)
{
  const uint64_t value = static_cast<uint64_t>(sequence);
  for (size_t index = 0U; index < sizeof(value); ++index) {
    output[index] = static_cast<uint8_t>(value >> (56U - 8U * index));
  }
}

int64_t decode_sequence(const uint8_t * input)
{
  uint64_t value = 0U;
  for (size_t index = 0U; index < sizeof(value); ++index) {
    value = (value << 8U) | input[index];
  }
  return static_cast<int64_t>(value);
}

rmw_ret_t publish_service_message(
  const rmw_publisher_t * publisher,
  const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t * callbacks,
  const void * ros_message, const rmw_request_id_t & request_id, rcutils_allocator_t allocator)
{
  auto payload = rmw_get_zero_initialized_serialized_message();
  rmw_ret_t result = rmw_serialized_message_init(&payload, 0U, &allocator);
  if (result != RMW_RET_OK) {return result;}
  const auto handle = message_handle(callbacks);
  result = rmw_serialize(ros_message, &handle, &payload);
  if (result == RMW_RET_OK &&
    payload.buffer_length > std::numeric_limits<size_t>::max() - request_header_size)
  {
    RMW_SET_ERROR_MSG("serialized service message is too large");
    result = RMW_RET_ERROR;
  }
  auto wire = rmw_get_zero_initialized_serialized_message();
  if (result == RMW_RET_OK) {
    result = rmw_serialized_message_init(
      &wire, request_header_size + payload.buffer_length, &allocator);
  }
  if (result == RMW_RET_OK) {
    std::memcpy(wire.buffer, request_id.writer_guid, dds_guid_size);
    encode_sequence(wire.buffer + dds_guid_size, request_id.sequence_number);
    std::memcpy(wire.buffer + request_header_size, payload.buffer, payload.buffer_length);
    wire.buffer_length = request_header_size + payload.buffer_length;
    result = rmw_publish_serialized_message(publisher, &wire, nullptr);
  }
  if (wire.allocator.allocate != nullptr) {(void)rmw_serialized_message_fini(&wire);}
  (void)rmw_serialized_message_fini(&payload);
  return result;
}

rmw_ret_t take_service_message(
  const rmw_subscription_t * subscription,
  const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t * callbacks,
  rmw_service_info_t * service_info, void * ros_message, bool * taken,
  rcutils_allocator_t allocator, const uint8_t * expected_guid = nullptr)
{
  auto wire = rmw_get_zero_initialized_serialized_message();
  rmw_ret_t result = rmw_serialized_message_init(&wire, 0U, &allocator);
  if (result != RMW_RET_OK) {return result;}
  do {
    rmw_message_info_t message_info{};
    wire.buffer_length = 0U;
    result = rmw_take_serialized_message_with_info(
      subscription, &wire, taken, &message_info, nullptr);
    if (result != RMW_RET_OK || !*taken) {break;}
    if (wire.buffer_length <= request_header_size) {
      RMW_SET_ERROR_MSG("received a malformed service message");
      result = RMW_RET_ERROR;
      *taken = false;
      break;
    }
    if (expected_guid != nullptr &&
      std::memcmp(wire.buffer, expected_guid, dds_guid_size) != 0)
    {
      *taken = false;
      continue;
    } else {
      std::memset(service_info, 0, sizeof(*service_info));
      service_info->source_timestamp = message_info.source_timestamp;
      service_info->received_timestamp = message_info.received_timestamp;
      std::memcpy(service_info->request_id.writer_guid, wire.buffer, dds_guid_size);
      service_info->request_id.sequence_number = decode_sequence(wire.buffer + dds_guid_size);
      rmw_serialized_message_t payload{};
      payload.buffer = wire.buffer + request_header_size;
      payload.buffer_length = wire.buffer_length - request_header_size;
      payload.buffer_capacity = payload.buffer_length;
      payload.allocator = allocator;
      const auto handle = message_handle(callbacks);
      result = rmw_deserialize(&payload, &handle, ros_message);
      if (result != RMW_RET_OK) {*taken = false;}
    }
    break;
  } while (true);
  (void)rmw_serialized_message_fini(&wire);
  return result;
}

rosidl_message_type_support_t message_handle(
  const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t * callbacks)
{
  return {rosidl_typesupport_zzdds_cpp::typesupport_identifier, callbacks, nullptr,
    nullptr, nullptr, nullptr};
}

bool service_topics(const char * name, std::string & request, std::string & response)
{
  if (name == nullptr || name[0] != '/' || name[1] == '\0') {
    RMW_SET_ERROR_MSG("a non-empty absolute service name is required");
    return false;
  }
  try {
    request = "/rq" + std::string(name) + "Request";
    response = "/rr" + std::string(name) + "Reply";
    return true;
  } catch (...) {
    RMW_SET_ERROR_MSG("failed to allocate service topic names");
    return false;
  }
}

template<typename Handle, typename Impl>
Handle * allocate_handle(ContextImpl * context, const char * name, Impl * impl)
{
  const auto allocator = context->allocator;
  auto * result = rmw_zzdds_cpp::allocate_object<Handle>(allocator);
  char * copy = rmw_zzdds_cpp::duplicate_string(allocator, name);
  if (result == nullptr || copy == nullptr) {
    if (copy != nullptr) {allocator.deallocate(copy, allocator.state);}
    rmw_zzdds_cpp::deallocate_object(allocator, result);
    return nullptr;
  }
  result->implementation_identifier = rmw_zzdds_cpp::identifier;
  result->data = impl;
  result->service_name = copy;
  return result;
}

void cleanup_publisher(rmw_node_t * node, rmw_publisher_t * publisher)
{
  const rmw_ret_t result = rmw_destroy_publisher(node, publisher);
  (void)result;
}

void cleanup_subscription(rmw_node_t * node, rmw_subscription_t * subscription)
{
  const rmw_ret_t result = rmw_destroy_subscription(node, subscription);
  (void)result;
}

class ServiceHashOverride final
{
public:
  explicit ServiceHashOverride(const rosidl_type_hash_t * hash)
  {
    rmw_zzdds_cpp::set_service_type_hash_override(hash);
  }
  ~ServiceHashOverride()
  {
    rmw_zzdds_cpp::set_service_type_hash_override(nullptr);
  }
  ServiceHashOverride(const ServiceHashOverride &) = delete;
  ServiceHashOverride & operator=(const ServiceHashOverride &) = delete;
};
}  // namespace

extern "C"
{
rmw_client_t * rmw_create_client(
  const rmw_node_t * node, const rosidl_service_type_support_t * type_support,
  const char * service_name, const rmw_qos_profile_t * qos)
{
  if (!valid_node(node) || qos == nullptr) {
    RMW_SET_ERROR_MSG("a valid node and QoS profile are required");
    return nullptr;
  }
  ServiceTypeSupport ts{};
  std::string request_topic, response_topic;
  if (!rmw_zzdds_cpp::resolve_service_type_support(type_support, &ts) ||
    !service_topics(service_name, request_topic, response_topic)) {return nullptr;}
  auto * context = reinterpret_cast<ContextImpl *>(node->context->impl);
  auto request_handle = message_handle(ts.request);
  auto response_handle = message_handle(ts.response);
  const ServiceHashOverride service_hash_override(&ts.type_hash);
  const auto pub_options = rmw_get_default_publisher_options();
  const auto sub_options = rmw_get_default_subscription_options();
  auto * pub = rmw_create_publisher(node, &request_handle, request_topic.c_str(), qos, &pub_options);
  if (pub == nullptr) {return nullptr;}
  auto * sub = rmw_create_subscription(node, &response_handle, response_topic.c_str(), qos, &sub_options);
  if (sub == nullptr) {
    cleanup_publisher(const_cast<rmw_node_t *>(node), pub);
    return nullptr;
  }
  static_cast<PublisherImpl *>(pub->data)->is_service_endpoint = true;
  static_cast<SubscriptionImpl *>(sub->data)->is_service_endpoint = true;
  auto * impl = rmw_zzdds_cpp::allocate_object<ClientImpl>(context->allocator,
    context, node, pub, sub, ts);
  auto * result = impl == nullptr ? nullptr : allocate_handle<rmw_client_t>(context, service_name, impl);
  if (result == nullptr) {
    rmw_zzdds_cpp::deallocate_object(context->allocator, impl);
    cleanup_subscription(const_cast<rmw_node_t *>(node), sub);
    cleanup_publisher(const_cast<rmw_node_t *>(node), pub);
    RMW_SET_ERROR_MSG("failed to allocate client resources");
  } else {
    try {
      const std::lock_guard<std::mutex> lock(context->mutex);
      context->clients.insert(result);
      context->notify_graph_change();
    } catch (const std::bad_alloc &) {
      cleanup_subscription(const_cast<rmw_node_t *>(node), sub);
      cleanup_publisher(const_cast<rmw_node_t *>(node), pub);
      context->allocator.deallocate(const_cast<char *>(result->service_name), context->allocator.state);
      rmw_zzdds_cpp::deallocate_object(context->allocator, impl);
      rmw_zzdds_cpp::deallocate_object(context->allocator, result);
      RMW_SET_ERROR_MSG("failed to register the client in the local graph");
      return nullptr;
    }
  }
  return result;
}

rmw_service_t * rmw_create_service(
  const rmw_node_t * node, const rosidl_service_type_support_t * type_support,
  const char * service_name, const rmw_qos_profile_t * qos)
{
  if (!valid_node(node) || qos == nullptr) {RMW_SET_ERROR_MSG("a valid node and QoS profile are required"); return nullptr;}
  ServiceTypeSupport ts{};
  std::string request_topic, response_topic;
  if (!rmw_zzdds_cpp::resolve_service_type_support(type_support, &ts) ||
    !service_topics(service_name, request_topic, response_topic)) {return nullptr;}
  auto * context = reinterpret_cast<ContextImpl *>(node->context->impl);
  auto request_handle = message_handle(ts.request);
  auto response_handle = message_handle(ts.response);
  const ServiceHashOverride service_hash_override(&ts.type_hash);
  const auto pub_options = rmw_get_default_publisher_options();
  const auto sub_options = rmw_get_default_subscription_options();
  auto * sub = rmw_create_subscription(node, &request_handle, request_topic.c_str(), qos, &sub_options);
  if (sub == nullptr) {return nullptr;}
  auto * pub = rmw_create_publisher(node, &response_handle, response_topic.c_str(), qos, &pub_options);
  if (pub == nullptr) {
    cleanup_subscription(const_cast<rmw_node_t *>(node), sub);
    return nullptr;
  }
  static_cast<SubscriptionImpl *>(sub->data)->is_service_endpoint = true;
  static_cast<PublisherImpl *>(pub->data)->is_service_endpoint = true;
  auto * impl = rmw_zzdds_cpp::allocate_object<ServiceImpl>(context->allocator,
    context, node, sub, pub, ts);
  auto * result = impl == nullptr ? nullptr : allocate_handle<rmw_service_t>(context, service_name, impl);
  if (result == nullptr) {
    rmw_zzdds_cpp::deallocate_object(context->allocator, impl);
    cleanup_publisher(const_cast<rmw_node_t *>(node), pub);
    cleanup_subscription(const_cast<rmw_node_t *>(node), sub);
    RMW_SET_ERROR_MSG("failed to allocate service resources");
  } else {
    try {
      const std::lock_guard<std::mutex> lock(context->mutex);
      context->services.insert(result);
      context->notify_graph_change();
    } catch (const std::bad_alloc &) {
      cleanup_publisher(const_cast<rmw_node_t *>(node), pub);
      cleanup_subscription(const_cast<rmw_node_t *>(node), sub);
      context->allocator.deallocate(const_cast<char *>(result->service_name), context->allocator.state);
      rmw_zzdds_cpp::deallocate_object(context->allocator, impl);
      rmw_zzdds_cpp::deallocate_object(context->allocator, result);
      RMW_SET_ERROR_MSG("failed to register the service in the local graph");
      return nullptr;
    }
  }
  return result;
}

rmw_ret_t rmw_destroy_client(rmw_node_t * node, rmw_client_t * client)
{
  if (node == nullptr || client == nullptr || client->data == nullptr) {return RMW_RET_INVALID_ARGUMENT;}
  if (node->implementation_identifier != rmw_zzdds_cpp::identifier ||
    client->implementation_identifier != rmw_zzdds_cpp::identifier) {return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;}
  auto * impl = static_cast<ClientImpl *>(client->data);
  auto allocator = impl->context->allocator;
  {
    const std::lock_guard<std::mutex> lock(impl->context->mutex);
    impl->context->clients.erase(client);
    impl->context->notify_graph_change();
  }
  rmw_ret_t result = rmw_destroy_subscription(node, impl->response_subscription);
  if (rmw_destroy_publisher(node, impl->request_publisher) != RMW_RET_OK) {result = RMW_RET_ERROR;}
  allocator.deallocate(const_cast<char *>(client->service_name), allocator.state);
  rmw_zzdds_cpp::deallocate_object(allocator, impl);
  rmw_zzdds_cpp::deallocate_object(allocator, client);
  return result;
}

rmw_ret_t rmw_destroy_service(rmw_node_t * node, rmw_service_t * service)
{
  if (node == nullptr || service == nullptr || service->data == nullptr) {return RMW_RET_INVALID_ARGUMENT;}
  if (node->implementation_identifier != rmw_zzdds_cpp::identifier ||
    service->implementation_identifier != rmw_zzdds_cpp::identifier) {return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;}
  auto * impl = static_cast<ServiceImpl *>(service->data);
  auto allocator = impl->context->allocator;
  {
    const std::lock_guard<std::mutex> lock(impl->context->mutex);
    impl->context->services.erase(service);
    impl->context->notify_graph_change();
  }
  rmw_ret_t result = rmw_destroy_publisher(node, impl->response_publisher);
  if (rmw_destroy_subscription(node, impl->request_subscription) != RMW_RET_OK) {result = RMW_RET_ERROR;}
  allocator.deallocate(const_cast<char *>(service->service_name), allocator.state);
  rmw_zzdds_cpp::deallocate_object(allocator, impl);
  rmw_zzdds_cpp::deallocate_object(allocator, service);
  return result;
}

rmw_ret_t rmw_send_request(
  const rmw_client_t * client, const void * ros_request, int64_t * sequence_id)
{
  if (!valid_handle(client)) {
    RMW_SET_ERROR_MSG("a valid client is required");
    return client != nullptr && client->implementation_identifier != rmw_zzdds_cpp::identifier ?
           RMW_RET_INCORRECT_RMW_IMPLEMENTATION : RMW_RET_INVALID_ARGUMENT;
  }
  if (ros_request == nullptr || sequence_id == nullptr) {
    RMW_SET_ERROR_MSG("a request message and sequence output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<ClientImpl *>(client->data);
  rmw_gid_t gid{};
  rmw_ret_t result = rmw_get_gid_for_publisher(impl->request_publisher, &gid);
  if (result != RMW_RET_OK) {return result;}
  rmw_request_id_t request_id{};
  std::memcpy(request_id.writer_guid, gid.data, dds_guid_size);
  request_id.sequence_number = impl->next_sequence.fetch_add(1, std::memory_order_relaxed);
  result = publish_service_message(
    impl->request_publisher, impl->typesupport.request, ros_request, request_id,
    impl->context->allocator);
  if (result == RMW_RET_OK) {*sequence_id = request_id.sequence_number;}
  return result;
}

rmw_ret_t rmw_take_request(
  const rmw_service_t * service, rmw_service_info_t * request_header,
  void * ros_request, bool * taken)
{
  if (!valid_handle(service)) {
    RMW_SET_ERROR_MSG("a valid service is required");
    return service != nullptr && service->implementation_identifier != rmw_zzdds_cpp::identifier ?
           RMW_RET_INCORRECT_RMW_IMPLEMENTATION : RMW_RET_INVALID_ARGUMENT;
  }
  if (request_header == nullptr || ros_request == nullptr || taken == nullptr) {
    RMW_SET_ERROR_MSG("request header, message, and taken output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<ServiceImpl *>(service->data);
  return take_service_message(
    impl->request_subscription, impl->typesupport.request, request_header, ros_request, taken,
    impl->context->allocator);
}

rmw_ret_t rmw_send_response(
  const rmw_service_t * service, rmw_request_id_t * request_id, void * ros_response)
{
  if (!valid_handle(service)) {
    RMW_SET_ERROR_MSG("a valid service is required");
    return service != nullptr && service->implementation_identifier != rmw_zzdds_cpp::identifier ?
           RMW_RET_INCORRECT_RMW_IMPLEMENTATION : RMW_RET_INVALID_ARGUMENT;
  }
  if (request_id == nullptr || ros_response == nullptr) {
    RMW_SET_ERROR_MSG("a request identifier and response message are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<ServiceImpl *>(service->data);
  return publish_service_message(
    impl->response_publisher, impl->typesupport.response, ros_response, *request_id,
    impl->context->allocator);
}

rmw_ret_t rmw_take_response(
  const rmw_client_t * client, rmw_service_info_t * request_header,
  void * ros_response, bool * taken)
{
  if (!valid_handle(client)) {
    RMW_SET_ERROR_MSG("a valid client is required");
    return client != nullptr && client->implementation_identifier != rmw_zzdds_cpp::identifier ?
           RMW_RET_INCORRECT_RMW_IMPLEMENTATION : RMW_RET_INVALID_ARGUMENT;
  }
  if (request_header == nullptr || ros_response == nullptr || taken == nullptr) {
    RMW_SET_ERROR_MSG("response header, message, and taken output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<ClientImpl *>(client->data);
  rmw_gid_t client_gid{};
  rmw_ret_t result = rmw_get_gid_for_publisher(impl->request_publisher, &client_gid);
  if (result != RMW_RET_OK) {return result;}
  return take_service_message(
    impl->response_subscription, impl->typesupport.response, request_header, ros_response, taken,
    impl->context->allocator, client_gid.data);
}

rmw_ret_t rmw_service_server_is_available(
  const rmw_node_t * node, const rmw_client_t * client, bool * is_available)
{
  if (node == nullptr || client == nullptr || is_available == nullptr) {
    RMW_SET_ERROR_MSG("a node, client, and availability output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (node->implementation_identifier != rmw_zzdds_cpp::identifier ||
    client->implementation_identifier != rmw_zzdds_cpp::identifier)
  {
    RMW_SET_ERROR_MSG("node or client belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (node->data == nullptr || client->data == nullptr) {
    RMW_SET_ERROR_MSG("node and client data are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<ClientImpl *>(client->data);
  size_t request_readers = 0U;
  size_t response_writers = 0U;
  rmw_ret_t result = rmw_publisher_count_matched_subscriptions(
    impl->request_publisher, &request_readers);
  if (result != RMW_RET_OK) {return result;}
  result = rmw_subscription_count_matched_publishers(
    impl->response_subscription, &response_writers);
  if (result != RMW_RET_OK) {return result;}
  *is_available = request_readers > 0U && response_writers > 0U;
  return RMW_RET_OK;
}

rmw_ret_t rmw_client_request_publisher_get_actual_qos(
  const rmw_client_t * client, rmw_qos_profile_t * qos)
{
  if (!valid_handle(client)) {return RMW_RET_INVALID_ARGUMENT;}
  return rmw_publisher_get_actual_qos(
    static_cast<ClientImpl *>(client->data)->request_publisher, qos);
}

rmw_ret_t rmw_client_response_subscription_get_actual_qos(
  const rmw_client_t * client, rmw_qos_profile_t * qos)
{
  if (!valid_handle(client)) {return RMW_RET_INVALID_ARGUMENT;}
  return rmw_subscription_get_actual_qos(
    static_cast<ClientImpl *>(client->data)->response_subscription, qos);
}

rmw_ret_t rmw_service_response_publisher_get_actual_qos(
  const rmw_service_t * service, rmw_qos_profile_t * qos)
{
  if (!valid_handle(service)) {return RMW_RET_INVALID_ARGUMENT;}
  return rmw_publisher_get_actual_qos(
    static_cast<ServiceImpl *>(service->data)->response_publisher, qos);
}

rmw_ret_t rmw_service_request_subscription_get_actual_qos(
  const rmw_service_t * service, rmw_qos_profile_t * qos)
{
  if (!valid_handle(service)) {return RMW_RET_INVALID_ARGUMENT;}
  return rmw_subscription_get_actual_qos(
    static_cast<ServiceImpl *>(service->data)->request_subscription, qos);
}

rmw_ret_t rmw_get_gid_for_client(const rmw_client_t * client, rmw_gid_t * gid)
{
  if (client == nullptr || client->data == nullptr) {return RMW_RET_INVALID_ARGUMENT;}
  if (client->implementation_identifier != rmw_zzdds_cpp::identifier) {
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  return rmw_get_gid_for_publisher(
    static_cast<ClientImpl *>(client->data)->request_publisher, gid);
}
}
