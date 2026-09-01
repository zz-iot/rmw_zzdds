#include <chrono>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>

#include "rcutils/allocator.h"
#include "rmw/enclave.h"
#include "rmw/error_handling.h"
#include "rmw/rmw.h"
#include "rmw_zzdds_test/srv/round_trip.hpp"
#include "rmw_zzdds_test/srv/detail/round_trip__type_support.hpp"
#include "rosidl_typesupport_interface/macros.h"

namespace
{

TEST(ServiceTransport, request_response_round_trip_preserves_identity_and_payload)
{
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rmw_init_options_t options = rmw_get_zero_initialized_init_options();
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&options, allocator));
  ASSERT_EQ(RMW_RET_OK, rmw_enclave_options_copy("/test", &allocator, &options.enclave));
  rmw_context_t context = rmw_get_zero_initialized_context();
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options, &context)) << rmw_get_error_string().str;
  rmw_node_t * node = rmw_create_node(&context, "service_round_trip", "/rmw_zzdds");
  ASSERT_NE(nullptr, node) << rmw_get_error_string().str;

  const auto * type_support = ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
    rosidl_typesupport_cpp, rmw_zzdds_test, srv, RoundTrip)();
  rmw_service_t * service = rmw_create_service(
    node, type_support, "/round_trip", &rmw_qos_profile_services_default);
  rmw_client_t * client = rmw_create_client(
    node, type_support, "/round_trip", &rmw_qos_profile_services_default);
  ASSERT_NE(nullptr, service) << rmw_get_error_string().str;
  ASSERT_NE(nullptr, client) << rmw_get_error_string().str;

  bool available = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && !available) {
    ASSERT_EQ(RMW_RET_OK, rmw_service_server_is_available(node, client, &available));
    if (!available) {std::this_thread::sleep_for(std::chrono::milliseconds(10));}
  }
  ASSERT_TRUE(available);

  rmw_zzdds_test::srv::RoundTrip::Request request;
  request.value = 21;
  request.label = "request payload";
  int64_t sequence = 0;
  ASSERT_EQ(RMW_RET_OK, rmw_send_request(client, &request, &sequence));
  ASSERT_GT(sequence, 0);

  rmw_wait_set_t * wait_set = rmw_create_wait_set(&context, 1U);
  ASSERT_NE(nullptr, wait_set);
  const rmw_time_t timeout{10U, 0U};
  void * service_entries[] = {service->data};
  rmw_services_t services{1U, service_entries};
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_wait(nullptr, nullptr, &services, nullptr, nullptr, wait_set, &timeout));
  ASSERT_EQ(service->data, service_entries[0]);

  rmw_service_info_t request_info{};
  rmw_zzdds_test::srv::RoundTrip::Request received_request;
  bool taken = false;
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_take_request(service, &request_info, &received_request, &taken));
  ASSERT_TRUE(taken);
  EXPECT_EQ(sequence, request_info.request_id.sequence_number);
  EXPECT_EQ(request.value, received_request.value);
  EXPECT_EQ(request.label, received_request.label);

  rmw_gid_t client_gid{};
  ASSERT_EQ(RMW_RET_OK, rmw_get_gid_for_client(client, &client_gid));
  EXPECT_EQ(
    0, std::memcmp(request_info.request_id.writer_guid, client_gid.data, 16U));

  rmw_zzdds_test::srv::RoundTrip::Response response;
  response.result = received_request.value * 2;
  response.label = received_request.label + " response";
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_send_response(service, &request_info.request_id, &response));

  void * client_entries[] = {client->data};
  rmw_clients_t clients{1U, client_entries};
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_wait(nullptr, nullptr, nullptr, &clients, nullptr, wait_set, &timeout));
  ASSERT_EQ(client->data, client_entries[0]);

  rmw_service_info_t response_info{};
  rmw_zzdds_test::srv::RoundTrip::Response received_response;
  taken = false;
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_take_response(client, &response_info, &received_response, &taken));
  ASSERT_TRUE(taken);
  EXPECT_EQ(sequence, response_info.request_id.sequence_number);
  EXPECT_EQ(
    0, std::memcmp(
      request_info.request_id.writer_guid,
      response_info.request_id.writer_guid, RMW_GID_STORAGE_SIZE));
  EXPECT_EQ(response.result, received_response.result);
  EXPECT_EQ(response.label, received_response.label);
  EXPECT_GT(request_info.received_timestamp, 0);
  EXPECT_GT(response_info.received_timestamp, 0);

  // Both clients begin their local sequence at one. Responses are broadcast on the
  // DDS reply topic, so the writer GUID must disambiguate them even when replies arrive reversed.
  rmw_client_t * second_client = rmw_create_client(
    node, type_support, "/round_trip", &rmw_qos_profile_services_default);
  ASSERT_NE(nullptr, second_client);
  available = false;
  while (std::chrono::steady_clock::now() < deadline && !available) {
    ASSERT_EQ(RMW_RET_OK, rmw_service_server_is_available(node, second_client, &available));
    if (!available) {std::this_thread::sleep_for(std::chrono::milliseconds(10));}
  }
  ASSERT_TRUE(available);

  rmw_zzdds_test::srv::RoundTrip::Request first_request;
  first_request.value = 31;
  first_request.label = "first client";
  rmw_zzdds_test::srv::RoundTrip::Request second_request;
  second_request.value = 41;
  second_request.label = "second client";
  int64_t first_sequence = 0;
  int64_t second_sequence = 0;
  ASSERT_EQ(RMW_RET_OK, rmw_send_request(client, &first_request, &first_sequence));
  ASSERT_EQ(RMW_RET_OK, rmw_send_request(second_client, &second_request, &second_sequence));
  EXPECT_EQ(2, first_sequence);
  EXPECT_EQ(1, second_sequence);

  rmw_service_info_t first_info{};
  rmw_service_info_t second_info{};
  for (size_t index = 0; index < 2U; ++index) {
    service_entries[0] = service->data;
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_wait(nullptr, nullptr, &services, nullptr, nullptr, wait_set, &timeout));
    rmw_service_info_t info{};
    rmw_zzdds_test::srv::RoundTrip::Request received;
    taken = false;
    ASSERT_EQ(RMW_RET_OK, rmw_take_request(service, &info, &received, &taken));
    ASSERT_TRUE(taken);
    if (received.label == first_request.label) {
      first_info = info;
    } else {
      ASSERT_EQ(second_request.label, received.label);
      second_info = info;
    }
  }

  rmw_zzdds_test::srv::RoundTrip::Response first_response;
  first_response.result = 62;
  first_response.label = "first response";
  rmw_zzdds_test::srv::RoundTrip::Response second_response;
  second_response.result = 82;
  second_response.label = "second response";
  ASSERT_EQ(RMW_RET_OK, rmw_send_response(service, &second_info.request_id, &second_response));
  ASSERT_EQ(RMW_RET_OK, rmw_send_response(service, &first_info.request_id, &first_response));

  client_entries[0] = client->data;
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_wait(nullptr, nullptr, nullptr, &clients, nullptr, wait_set, &timeout));
  rmw_zzdds_test::srv::RoundTrip::Response first_received;
  taken = false;
  while (std::chrono::steady_clock::now() < deadline && !taken) {
    client_entries[0] = client->data;
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_wait(nullptr, nullptr, nullptr, &clients, nullptr, wait_set, &timeout));
    ASSERT_EQ(RMW_RET_OK, rmw_take_response(client, &response_info, &first_received, &taken));
  }
  ASSERT_TRUE(taken);
  EXPECT_EQ(first_sequence, response_info.request_id.sequence_number);
  EXPECT_EQ(first_response.label, first_received.label);

  void * second_client_entries[] = {second_client->data};
  rmw_clients_t second_clients{1U, second_client_entries};
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_wait(nullptr, nullptr, nullptr, &second_clients, nullptr, wait_set, &timeout));
  rmw_zzdds_test::srv::RoundTrip::Response second_received;
  taken = false;
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_take_response(second_client, &response_info, &second_received, &taken));
  ASSERT_TRUE(taken);
  EXPECT_EQ(second_sequence, response_info.request_id.sequence_number);
  EXPECT_EQ(second_response.label, second_received.label);

  EXPECT_EQ(RMW_RET_OK, rmw_destroy_wait_set(wait_set));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_client(node, second_client));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_client(node, client));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_service(node, service));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(node));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&options));
}

}  // namespace
