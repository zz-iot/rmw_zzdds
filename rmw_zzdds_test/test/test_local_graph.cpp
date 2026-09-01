#include <chrono>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>

#include "rcutils/allocator.h"
#include "rmw/enclave.h"
#include "rmw/error_handling.h"
#include "rmw/get_node_info_and_types.h"
#include "rmw/get_service_endpoint_info.h"
#include "rmw/get_service_names_and_types.h"
#include "rmw/get_topic_endpoint_info.h"
#include "rmw/get_topic_names_and_types.h"
#include "rmw/names_and_types.h"
#include "rmw/rmw.h"
#include "rmw_zzdds_test/msg/primitive_record.hpp"
#include "rmw_zzdds_test/srv/round_trip.hpp"
#include "rmw_zzdds_test/srv/detail/round_trip__type_support.hpp"
#include "rosidl_typesupport_interface/macros.h"
#include "rosidl_typesupport_zzdds_cpp/message_type_support.hpp"

namespace
{
bool contains(
  const rmw_names_and_types_t & values, const char * name, const char * type)
{
  for (size_t i = 0U; i < values.names.size; ++i) {
    if (std::strcmp(values.names.data[i], name) != 0) {continue;}
    for (size_t j = 0U; j < values.types[i].size; ++j) {
      if (std::strcmp(values.types[i].data[j], type) == 0) {return true;}
    }
  }
  return false;
}

template<typename CountFunction>
bool wait_for_count(
  CountFunction count_function, const rmw_node_t * node, const char * topic_name,
  size_t expected)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    size_t count = 0U;
    if (count_function(node, topic_name, &count) != RMW_RET_OK) {return false;}
    if (count == expected) {return true;}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

TEST(LocalGraph, reports_topics_services_and_node_ownership)
{
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rmw_init_options_t options = rmw_get_zero_initialized_init_options();
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&options, allocator));
  ASSERT_EQ(RMW_RET_OK, rmw_enclave_options_copy("/graph_test", &allocator, &options.enclave));
  rmw_context_t context = rmw_get_zero_initialized_context();
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options, &context));
  rmw_node_t * publisher_node = rmw_create_node(&context, "publisher_node", "/graph");
  rmw_node_t * subscription_node = rmw_create_node(&context, "subscription_node", "/graph");
  ASSERT_NE(nullptr, publisher_node);
  ASSERT_NE(nullptr, subscription_node);

  const auto * message_type = rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
    rmw_zzdds_test::msg::PrimitiveRecord>();
  const auto publisher_options = rmw_get_default_publisher_options();
  const auto subscription_options = rmw_get_default_subscription_options();
  rmw_publisher_t * publisher = rmw_create_publisher(
    publisher_node, message_type, "/records", &rmw_qos_profile_default, &publisher_options);
  rmw_subscription_t * subscription = rmw_create_subscription(
    subscription_node, message_type, "/records", &rmw_qos_profile_default,
    &subscription_options);
  ASSERT_NE(nullptr, publisher);
  ASSERT_NE(nullptr, subscription);

  const auto * service_type = ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
    rosidl_typesupport_cpp, rmw_zzdds_test, srv, RoundTrip)();
  rmw_service_t * service = rmw_create_service(
    publisher_node, service_type, "/round_trip", &rmw_qos_profile_services_default);
  rmw_client_t * client = rmw_create_client(
    subscription_node, service_type, "/round_trip", &rmw_qos_profile_services_default);
  ASSERT_NE(nullptr, service);
  ASSERT_NE(nullptr, client);

  EXPECT_EQ(RMW_RET_ERROR, rmw_destroy_node(publisher_node));
  EXPECT_NE(
    nullptr,
    std::strstr(
      rmw_get_error_string().str,
      "node still owns publishers, subscriptions, clients, or services"));
  rmw_reset_error();

  rmw_names_and_types_t values = rmw_get_zero_initialized_names_and_types();
  ASSERT_EQ(RMW_RET_OK, rmw_get_topic_names_and_types(publisher_node, &allocator, false, &values));
  EXPECT_EQ(1U, values.names.size);
  EXPECT_TRUE(contains(values, "/records", "rmw_zzdds_test/msg/PrimitiveRecord"));
  ASSERT_EQ(RMW_RET_OK, rmw_names_and_types_fini(&values));

  values = rmw_get_zero_initialized_names_and_types();
  ASSERT_EQ(
    RMW_RET_OK, rmw_get_publisher_names_and_types_by_node(
      publisher_node, &allocator, "publisher_node", "/graph", false, &values));
  EXPECT_TRUE(contains(values, "/records", "rmw_zzdds_test/msg/PrimitiveRecord"));
  ASSERT_EQ(RMW_RET_OK, rmw_names_and_types_fini(&values));

  values = rmw_get_zero_initialized_names_and_types();
  ASSERT_EQ(RMW_RET_OK, rmw_get_service_names_and_types(publisher_node, &allocator, &values));
  EXPECT_TRUE(contains(values, "/round_trip", "rmw_zzdds_test/srv/RoundTrip"));
  ASSERT_EQ(RMW_RET_OK, rmw_names_and_types_fini(&values));

  rmw_topic_endpoint_info_array_t topic_info =
    rmw_get_zero_initialized_topic_endpoint_info_array();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_publishers_info_by_topic(
      publisher_node, &allocator, "/records", false, &topic_info));
  ASSERT_EQ(1U, topic_info.size);
  EXPECT_STREQ("publisher_node", topic_info.info_array[0].node_name);
  EXPECT_STREQ("/graph", topic_info.info_array[0].node_namespace);
  EXPECT_STREQ("rmw_zzdds_test/msg/PrimitiveRecord", topic_info.info_array[0].topic_type);
  EXPECT_NE(ROSIDL_TYPE_HASH_VERSION_UNSET, topic_info.info_array[0].topic_type_hash.version);
  EXPECT_EQ(RMW_ENDPOINT_PUBLISHER, topic_info.info_array[0].endpoint_type);
  EXPECT_EQ(rmw_qos_profile_default.reliability, topic_info.info_array[0].qos_profile.reliability);
  rmw_gid_t publisher_gid{};
  ASSERT_EQ(RMW_RET_OK, rmw_get_gid_for_publisher(publisher, &publisher_gid));
  EXPECT_EQ(
    0, std::memcmp(
      publisher_gid.data, topic_info.info_array[0].endpoint_gid, RMW_GID_STORAGE_SIZE));
  ASSERT_EQ(RMW_RET_OK, rmw_topic_endpoint_info_array_fini(&topic_info, &allocator));

  topic_info = rmw_get_zero_initialized_topic_endpoint_info_array();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_subscriptions_info_by_topic(
      publisher_node, &allocator, "/records", false, &topic_info));
  ASSERT_EQ(1U, topic_info.size);
  EXPECT_STREQ("subscription_node", topic_info.info_array[0].node_name);
  EXPECT_EQ(RMW_ENDPOINT_SUBSCRIPTION, topic_info.info_array[0].endpoint_type);
  ASSERT_EQ(RMW_RET_OK, rmw_topic_endpoint_info_array_fini(&topic_info, &allocator));

  rmw_service_endpoint_info_array_t service_info =
    rmw_get_zero_initialized_service_endpoint_info_array();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_clients_info_by_service(
      publisher_node, &allocator, "/round_trip", false, &service_info));
  ASSERT_EQ(1U, service_info.size);
  EXPECT_STREQ("subscription_node", service_info.info_array[0].node_name);
  EXPECT_STREQ("rmw_zzdds_test/srv/RoundTrip", service_info.info_array[0].service_type);
  EXPECT_NE(ROSIDL_TYPE_HASH_VERSION_UNSET, service_info.info_array[0].service_type_hash.version);
  EXPECT_EQ(RMW_ENDPOINT_CLIENT, service_info.info_array[0].endpoint_type);
  ASSERT_EQ(2U, service_info.info_array[0].endpoint_count);
  rmw_gid_t client_gid{};
  ASSERT_EQ(RMW_RET_OK, rmw_get_gid_for_client(client, &client_gid));
  // DDS service endpoint ordering is reader followed by writer. A client's public GID is
  // its request writer, so it must be the second entry.
  EXPECT_EQ(
    0, std::memcmp(
      client_gid.data, service_info.info_array[0].endpoint_gids[1], RMW_GID_STORAGE_SIZE));
  ASSERT_EQ(RMW_RET_OK, rmw_service_endpoint_info_array_fini(&service_info, &allocator));

  service_info = rmw_get_zero_initialized_service_endpoint_info_array();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_servers_info_by_service(
      publisher_node, &allocator, "/round_trip", false, &service_info));
  ASSERT_EQ(1U, service_info.size);
  EXPECT_STREQ("publisher_node", service_info.info_array[0].node_name);
  EXPECT_EQ(RMW_ENDPOINT_SERVER, service_info.info_array[0].endpoint_type);
  EXPECT_EQ(2U, service_info.info_array[0].endpoint_count);
  ASSERT_EQ(RMW_RET_OK, rmw_service_endpoint_info_array_fini(&service_info, &allocator));

  EXPECT_EQ(RMW_RET_OK, rmw_destroy_client(subscription_node, client));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_service(publisher_node, service));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_subscription(subscription_node, subscription));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(publisher_node, publisher));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(subscription_node));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(publisher_node));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&options));
}

TEST(LocalGraph, counts_discovered_endpoints_and_removes_stale_entries)
{
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rmw_init_options_t observer_options = rmw_get_zero_initialized_init_options();
  rmw_init_options_t remote_options = rmw_get_zero_initialized_init_options();
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&observer_options, allocator));
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&remote_options, allocator));
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_enclave_options_copy("/observer", &allocator, &observer_options.enclave));
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_enclave_options_copy("/remote", &allocator, &remote_options.enclave));
  rmw_context_t observer_context = rmw_get_zero_initialized_context();
  rmw_context_t remote_context = rmw_get_zero_initialized_context();
  ASSERT_EQ(RMW_RET_OK, rmw_init(&observer_options, &observer_context));
  ASSERT_EQ(RMW_RET_OK, rmw_init(&remote_options, &remote_context));
  rmw_node_t * observer_node = rmw_create_node(&observer_context, "observer", "/graph");
  rmw_node_t * remote_node = rmw_create_node(&remote_context, "remote", "/graph");
  ASSERT_NE(nullptr, observer_node);
  ASSERT_NE(nullptr, remote_node);

  const auto * message_type = rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
    rmw_zzdds_test::msg::PrimitiveRecord>();
  const auto publisher_options = rmw_get_default_publisher_options();
  const auto subscription_options = rmw_get_default_subscription_options();
  rmw_publisher_t * publisher = rmw_create_publisher(
    remote_node, message_type, "/discovered_records", &rmw_qos_profile_default,
    &publisher_options);
  ASSERT_NE(nullptr, publisher);
  EXPECT_TRUE(wait_for_count(rmw_count_publishers, observer_node, "/discovered_records", 1U));
  rmw_topic_endpoint_info_array_t endpoint_info =
    rmw_get_zero_initialized_topic_endpoint_info_array();
  const auto association_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_get_publishers_info_by_topic(
        observer_node, &allocator, "/discovered_records", false, &endpoint_info));
    if (endpoint_info.size == 1U &&
      std::strcmp(endpoint_info.info_array[0].node_name, "remote") == 0)
    {
      break;
    }
    ASSERT_EQ(RMW_RET_OK, rmw_topic_endpoint_info_array_fini(&endpoint_info, &allocator));
    endpoint_info = rmw_get_zero_initialized_topic_endpoint_info_array();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < association_deadline);
  ASSERT_EQ(1U, endpoint_info.size);
  EXPECT_STREQ("remote", endpoint_info.info_array[0].node_name);
  EXPECT_STREQ("/graph", endpoint_info.info_array[0].node_namespace);
  EXPECT_STREQ(
    "rmw_zzdds_test/msg/PrimitiveRecord", endpoint_info.info_array[0].topic_type);
  EXPECT_NE(ROSIDL_TYPE_HASH_VERSION_UNSET, endpoint_info.info_array[0].topic_type_hash.version);
  rmw_gid_t publisher_gid{};
  ASSERT_EQ(RMW_RET_OK, rmw_get_gid_for_publisher(publisher, &publisher_gid));
  EXPECT_EQ(
    0, std::memcmp(
      publisher_gid.data, endpoint_info.info_array[0].endpoint_gid, RMW_GID_STORAGE_SIZE));
  ASSERT_EQ(RMW_RET_OK, rmw_topic_endpoint_info_array_fini(&endpoint_info, &allocator));

  rcutils_string_array_t node_names = rcutils_get_zero_initialized_string_array();
  rcutils_string_array_t node_namespaces = rcutils_get_zero_initialized_string_array();
  rcutils_string_array_t node_enclaves = rcutils_get_zero_initialized_string_array();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_node_names_with_enclaves(
      observer_node, &node_names, &node_namespaces, &node_enclaves));
  bool found_remote_node = false;
  for (size_t index = 0U; index < node_names.size; ++index) {
    if (std::strcmp(node_names.data[index], "remote") == 0 &&
      std::strcmp(node_namespaces.data[index], "/graph") == 0)
    {
      found_remote_node = true;
    }
  }
  EXPECT_TRUE(found_remote_node);
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_string_array_fini(&node_enclaves));
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_string_array_fini(&node_namespaces));
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_string_array_fini(&node_names));

  rmw_names_and_types_t remote_topics = rmw_get_zero_initialized_names_and_types();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_topic_names_and_types(observer_node, &allocator, false, &remote_topics));
  EXPECT_TRUE(contains(
      remote_topics, "/discovered_records", "rmw_zzdds_test/msg/PrimitiveRecord"));
  ASSERT_EQ(RMW_RET_OK, rmw_names_and_types_fini(&remote_topics));

  remote_topics = rmw_get_zero_initialized_names_and_types();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_publisher_names_and_types_by_node(
      observer_node, &allocator, "remote", "/graph", false, &remote_topics));
  EXPECT_TRUE(contains(
      remote_topics, "/discovered_records", "rmw_zzdds_test/msg/PrimitiveRecord"));
  ASSERT_EQ(RMW_RET_OK, rmw_names_and_types_fini(&remote_topics));
  ASSERT_EQ(RMW_RET_OK, rmw_destroy_publisher(remote_node, publisher));
  EXPECT_TRUE(wait_for_count(rmw_count_publishers, observer_node, "/discovered_records", 0U));

  rmw_subscription_t * subscription = rmw_create_subscription(
    remote_node, message_type, "/discovered_records", &rmw_qos_profile_default,
    &subscription_options);
  ASSERT_NE(nullptr, subscription);
  EXPECT_TRUE(wait_for_count(rmw_count_subscribers, observer_node, "/discovered_records", 1U));
  ASSERT_EQ(RMW_RET_OK, rmw_destroy_subscription(remote_node, subscription));
  EXPECT_TRUE(wait_for_count(rmw_count_subscribers, observer_node, "/discovered_records", 0U));

  const auto * service_type = ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
    rosidl_typesupport_cpp, rmw_zzdds_test, srv, RoundTrip)();
  rmw_service_t * remote_service = rmw_create_service(
    remote_node, service_type, "/discovered_round_trip", &rmw_qos_profile_services_default);
  ASSERT_NE(nullptr, remote_service);
  const bool request_reader_found = wait_for_count(
    rmw_count_subscribers, observer_node, "/rq/discovered_round_tripRequest", 1U);
  EXPECT_TRUE(request_reader_found);
  EXPECT_TRUE(wait_for_count(
      rmw_count_publishers, observer_node, "/rr/discovered_round_tripReply", 1U));
  rmw_service_endpoint_info_array_t service_info =
    rmw_get_zero_initialized_service_endpoint_info_array();
  const auto service_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_get_servers_info_by_service(
        observer_node, &allocator, "/discovered_round_trip", false, &service_info));
    if (service_info.size == 1U &&
      std::strcmp(service_info.info_array[0].node_name, "remote") == 0)
    {
      break;
    }
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_service_endpoint_info_array_fini(&service_info, &allocator));
    service_info = rmw_get_zero_initialized_service_endpoint_info_array();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < service_deadline);
  ASSERT_EQ(1U, service_info.size);
  EXPECT_STREQ("remote", service_info.info_array[0].node_name);
  EXPECT_STREQ("/graph", service_info.info_array[0].node_namespace);
  EXPECT_STREQ("rmw_zzdds_test/srv/RoundTrip", service_info.info_array[0].service_type);
  EXPECT_NE(
    ROSIDL_TYPE_HASH_VERSION_UNSET, service_info.info_array[0].service_type_hash.version);
  EXPECT_EQ(RMW_ENDPOINT_SERVER, service_info.info_array[0].endpoint_type);
  EXPECT_EQ(2U, service_info.info_array[0].endpoint_count);
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_service_endpoint_info_array_fini(&service_info, &allocator));

  rmw_names_and_types_t remote_services = rmw_get_zero_initialized_names_and_types();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_service_names_and_types(observer_node, &allocator, &remote_services));
  EXPECT_TRUE(contains(
      remote_services, "/discovered_round_trip", "rmw_zzdds_test/srv/RoundTrip"));
  ASSERT_EQ(RMW_RET_OK, rmw_names_and_types_fini(&remote_services));
  remote_services = rmw_get_zero_initialized_names_and_types();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_service_names_and_types_by_node(
      observer_node, &allocator, "remote", "/graph", &remote_services));
  EXPECT_TRUE(contains(
      remote_services, "/discovered_round_trip", "rmw_zzdds_test/srv/RoundTrip"));
  ASSERT_EQ(RMW_RET_OK, rmw_names_and_types_fini(&remote_services));
  EXPECT_TRUE(wait_for_count(
      rmw_count_services, observer_node, "/discovered_round_trip", 1U));
  ASSERT_EQ(RMW_RET_OK, rmw_destroy_service(remote_node, remote_service));
  EXPECT_TRUE(wait_for_count(
      rmw_count_services, observer_node, "/discovered_round_trip", 0U));

  rmw_client_t * remote_client = rmw_create_client(
    remote_node, service_type, "/discovered_client", &rmw_qos_profile_services_default);
  ASSERT_NE(nullptr, remote_client);
  rmw_service_endpoint_info_array_t client_info =
    rmw_get_zero_initialized_service_endpoint_info_array();
  const auto client_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_get_clients_info_by_service(
        observer_node, &allocator, "/discovered_client", false, &client_info));
    if (client_info.size == 1U &&
      std::strcmp(client_info.info_array[0].node_name, "remote") == 0)
    {
      break;
    }
    ASSERT_EQ(RMW_RET_OK, rmw_service_endpoint_info_array_fini(&client_info, &allocator));
    client_info = rmw_get_zero_initialized_service_endpoint_info_array();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < client_deadline);
  ASSERT_EQ(1U, client_info.size);
  EXPECT_STREQ("remote", client_info.info_array[0].node_name);
  EXPECT_STREQ("/graph", client_info.info_array[0].node_namespace);
  EXPECT_STREQ("rmw_zzdds_test/srv/RoundTrip", client_info.info_array[0].service_type);
  EXPECT_NE(ROSIDL_TYPE_HASH_VERSION_UNSET, client_info.info_array[0].service_type_hash.version);
  EXPECT_EQ(RMW_ENDPOINT_CLIENT, client_info.info_array[0].endpoint_type);
  EXPECT_EQ(2U, client_info.info_array[0].endpoint_count);
  ASSERT_EQ(RMW_RET_OK, rmw_service_endpoint_info_array_fini(&client_info, &allocator));

  rmw_names_and_types_t remote_clients = rmw_get_zero_initialized_names_and_types();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_client_names_and_types_by_node(
      observer_node, &allocator, "remote", "/graph", &remote_clients));
  EXPECT_TRUE(contains(
      remote_clients, "/discovered_client", "rmw_zzdds_test/srv/RoundTrip"));
  ASSERT_EQ(RMW_RET_OK, rmw_names_and_types_fini(&remote_clients));
  remote_services = rmw_get_zero_initialized_names_and_types();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_service_names_and_types(observer_node, &allocator, &remote_services));
  EXPECT_FALSE(contains(
      remote_services, "/discovered_client", "rmw_zzdds_test/srv/RoundTrip"));
  ASSERT_EQ(RMW_RET_OK, rmw_names_and_types_fini(&remote_services));
  EXPECT_TRUE(wait_for_count(rmw_count_clients, observer_node, "/discovered_client", 1U));
  ASSERT_EQ(RMW_RET_OK, rmw_destroy_client(remote_node, remote_client));
  EXPECT_TRUE(wait_for_count(rmw_count_clients, observer_node, "/discovered_client", 0U));

  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(remote_node));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(observer_node));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&remote_context));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&observer_context));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&remote_context));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&observer_context));
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&remote_options));
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&observer_options));
}
}  // namespace
