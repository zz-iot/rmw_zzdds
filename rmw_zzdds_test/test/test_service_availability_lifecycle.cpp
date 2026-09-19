#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "rcutils/allocator.h"
#include "rmw/enclave.h"
#include "rmw/error_handling.h"
#include "rmw/rmw.h"
#include "rmw_zzdds_cpp/service_impl.hpp"
#include "rmw_zzdds_test/srv/round_trip.hpp"
#include "rmw_zzdds_test/srv/detail/round_trip__type_support.hpp"
#include "rosidl_typesupport_interface/macros.h"

// Regression coverage for rmw_service_server_is_available's on_reliable_
// writer_ready-backed readiness tracking (PR #9 review): the count-based
// signal must go up when the service's response writer actually registers
// this client's response reader, back down when that writer departs, and
// survive an unrelated rmw_event_set_callback reinstalling the response
// subscription's listener (apply_subscription_listener replaces the whole
// zzdds_DataReaderListenerEx struct on every such call, including the
// always-on on_reliable_writer_ready tracker -- see rmw_event.cpp).

namespace
{

void count_events(const void * data, size_t count)
{
  static_cast<std::atomic_size_t *>(const_cast<void *>(data))->fetch_add(count);
}

struct NodeFixture
{
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rmw_init_options_t options = rmw_get_zero_initialized_init_options();
  rmw_context_t context = rmw_get_zero_initialized_context();
  rmw_node_t * node = nullptr;
};

testing::AssertionResult set_up(NodeFixture & fx, const char * enclave, const char * node_name)
{
  if (RMW_RET_OK != rmw_init_options_init(&fx.options, fx.allocator)) {
    return testing::AssertionFailure() << "rmw_init_options_init failed";
  }
  if (RMW_RET_OK != rmw_enclave_options_copy(enclave, &fx.allocator, &fx.options.enclave)) {
    return testing::AssertionFailure() << "rmw_enclave_options_copy failed";
  }
  if (RMW_RET_OK != rmw_init(&fx.options, &fx.context)) {
    return testing::AssertionFailure() << rmw_get_error_string().str;
  }
  fx.node = rmw_create_node(&fx.context, node_name, "/rmw_zzdds");
  if (fx.node == nullptr) {
    return testing::AssertionFailure() << rmw_get_error_string().str;
  }
  return testing::AssertionSuccess();
}

void tear_down(NodeFixture & fx)
{
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(fx.node));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&fx.context));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&fx.context));
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&fx.options));
}

const rosidl_service_type_support_t * round_trip_type_support()
{
  return ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
    rosidl_typesupport_cpp, rmw_zzdds_test, srv, RoundTrip)();
}

TEST(ServiceAvailability, unavailable_before_service_then_available_after_creation)
{
  NodeFixture fx;
  ASSERT_TRUE(set_up(fx, "/test", "availability_initial"));

  rmw_client_t * client = rmw_create_client(
    fx.node, round_trip_type_support(), "/lifecycle_initial", &rmw_qos_profile_services_default);
  ASSERT_NE(nullptr, client) << rmw_get_error_string().str;

  bool available = true;
  ASSERT_EQ(RMW_RET_OK, rmw_service_server_is_available(fx.node, client, &available));
  EXPECT_FALSE(available);

  rmw_service_t * service = rmw_create_service(
    fx.node, round_trip_type_support(), "/lifecycle_initial", &rmw_qos_profile_services_default);
  ASSERT_NE(nullptr, service) << rmw_get_error_string().str;

  available = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && !available) {
    ASSERT_EQ(RMW_RET_OK, rmw_service_server_is_available(fx.node, client, &available));
    if (!available) {std::this_thread::sleep_for(std::chrono::milliseconds(10));}
  }
  EXPECT_TRUE(available);

  EXPECT_EQ(RMW_RET_OK, rmw_destroy_service(fx.node, service));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_client(fx.node, client));
  tear_down(fx);
}

TEST(ServiceAvailability, becomes_unavailable_after_service_destroyed)
{
  NodeFixture fx;
  ASSERT_TRUE(set_up(fx, "/test", "availability_departure"));

  rmw_client_t * client = rmw_create_client(
    fx.node, round_trip_type_support(), "/lifecycle_departure", &rmw_qos_profile_services_default);
  ASSERT_NE(nullptr, client) << rmw_get_error_string().str;
  rmw_service_t * service = rmw_create_service(
    fx.node, round_trip_type_support(), "/lifecycle_departure", &rmw_qos_profile_services_default);
  ASSERT_NE(nullptr, service) << rmw_get_error_string().str;

  bool available = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && !available) {
    ASSERT_EQ(RMW_RET_OK, rmw_service_server_is_available(fx.node, client, &available));
    if (!available) {std::this_thread::sleep_for(std::chrono::milliseconds(10));}
  }
  ASSERT_TRUE(available);

  // The response writer departing (service destroyed, no SPDP BYE) must
  // eventually drive the readiness count back down -- proves the is_ready
  // == false half of reliable_writer_ready_listener's fetch_add actually
  // fires and is reflected here, not just the initial ready == true half.
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_service(fx.node, service));

  available = true;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && available) {
    ASSERT_EQ(RMW_RET_OK, rmw_service_server_is_available(fx.node, client, &available));
    if (available) {std::this_thread::sleep_for(std::chrono::milliseconds(10));}
  }
  EXPECT_FALSE(available);

  EXPECT_EQ(RMW_RET_OK, rmw_destroy_client(fx.node, client));
  tear_down(fx);
}

TEST(ServiceAvailability, readiness_survives_response_subscription_listener_reinstallation)
{
  NodeFixture fx;
  ASSERT_TRUE(set_up(fx, "/test", "availability_reinstall"));

  rmw_client_t * client = rmw_create_client(
    fx.node, round_trip_type_support(), "/lifecycle_reinstall", &rmw_qos_profile_services_default);
  ASSERT_NE(nullptr, client) << rmw_get_error_string().str;
  rmw_service_t * service = rmw_create_service(
    fx.node, round_trip_type_support(), "/lifecycle_reinstall", &rmw_qos_profile_services_default);
  ASSERT_NE(nullptr, service) << rmw_get_error_string().str;

  bool available = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && !available) {
    ASSERT_EQ(RMW_RET_OK, rmw_service_server_is_available(fx.node, client, &available));
    if (!available) {std::this_thread::sleep_for(std::chrono::milliseconds(10));}
  }
  ASSERT_TRUE(available);

  // Register an unrelated event callback on the client's internal response
  // subscription -- the same rmw_event_set_callback path any application
  // subscription event goes through, which reinstalls the whole zzdds
  // listener struct (apply_subscription_listener). Must not silently drop
  // the on_reliable_writer_ready readiness this test already observed.
  auto * client_impl = static_cast<rmw_zzdds_cpp::ClientImpl *>(client->data);
  ASSERT_NE(nullptr, client_impl);
  rmw_event_t event = rmw_get_zero_initialized_event();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_subscription_event_init(
      &event, client_impl->response_subscription, RMW_EVENT_LIVELINESS_CHANGED));
  std::atomic_size_t callback_count{0U};
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event, count_events, &callback_count));

  available = false;
  ASSERT_EQ(RMW_RET_OK, rmw_service_server_is_available(fx.node, client, &available));
  EXPECT_TRUE(available);

  // The gap the above alone leaves (Greptile PR #9 review): confirming
  // readiness merely *survived* reinstallation doesn't prove the newly
  // installed listener instance still correctly tracks a *subsequent*
  // transition -- e.g. a bug that reset the count, or that wired the new
  // struct's on_reliable_writer_ready to a stale/no-op closure, would look
  // identical up to this point. Drive one more real transition (the
  // response writer departing) through the post-reinstallation listener
  // and confirm it's still observed.
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_service(fx.node, service));
  service = nullptr;
  available = true;
  const auto departed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < departed_deadline && available) {
    ASSERT_EQ(RMW_RET_OK, rmw_service_server_is_available(fx.node, client, &available));
    if (available) {std::this_thread::sleep_for(std::chrono::milliseconds(10));}
  }
  EXPECT_FALSE(available);

  EXPECT_EQ(RMW_RET_OK, rmw_event_fini(&event));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_client(fx.node, client));
  tear_down(fx);
}

}  // namespace
