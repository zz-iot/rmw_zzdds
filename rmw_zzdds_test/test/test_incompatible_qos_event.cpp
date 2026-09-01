#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "rcutils/allocator.h"
#include "rmw/enclave.h"
#include "rmw/events_statuses/incompatible_qos.h"
#include "rmw/rmw.h"
#include "rmw_zzdds_test/msg/primitive_record.hpp"
#include "rosidl_typesupport_zzdds_cpp/message_type_support.hpp"

namespace
{
void count_events(const void * data, size_t count)
{
  static_cast<std::atomic_size_t *>(const_cast<void *>(data))->fetch_add(count);
}

TEST(EventTransport, requested_incompatible_qos_callback_wait_and_take)
{
  auto allocator = rcutils_get_default_allocator();
  auto options = rmw_get_zero_initialized_init_options();
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&options, allocator));
  ASSERT_EQ(RMW_RET_OK, rmw_enclave_options_copy("/test", &allocator, &options.enclave));
  auto context = rmw_get_zero_initialized_context();
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options, &context));
  rmw_node_t * node = rmw_create_node(&context, "incompatible_qos", "/rmw_zzdds");
  ASSERT_NE(nullptr, node);
  const auto * type_support = rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
    rmw_zzdds_test::msg::PrimitiveRecord>();

  auto subscription_qos = rmw_qos_profile_default;
  subscription_qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
  const auto subscription_options = rmw_get_default_subscription_options();
  rmw_subscription_t * subscription = rmw_create_subscription(
    node, type_support, "/incompatible_qos", &subscription_qos, &subscription_options);
  ASSERT_NE(nullptr, subscription);
  rmw_event_t event = rmw_get_zero_initialized_event();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_subscription_event_init(&event, subscription, RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE));
  std::atomic_size_t callback_count{0U};
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event, count_events, &callback_count));

  auto publisher_qos = rmw_qos_profile_default;
  publisher_qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
  const auto publisher_options = rmw_get_default_publisher_options();
  rmw_publisher_t * publisher = rmw_create_publisher(
    node, type_support, "/incompatible_qos", &publisher_qos, &publisher_options);
  ASSERT_NE(nullptr, publisher);

  rmw_wait_set_t * wait_set = rmw_create_wait_set(&context, 1U);
  ASSERT_NE(nullptr, wait_set);
  void * entries[] = {&event};
  rmw_events_t events{1U, entries};
  const rmw_time_t timeout{10U, 0U};
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_wait(nullptr, nullptr, nullptr, nullptr, &events, wait_set, &timeout));
  EXPECT_EQ(&event, entries[0]);
  rmw_requested_qos_incompatible_event_status_t status{};
  bool taken = false;
  ASSERT_EQ(RMW_RET_OK, rmw_take_event(&event, &status, &taken));
  EXPECT_TRUE(taken);
  EXPECT_GE(status.total_count, 1);
  EXPECT_GE(status.total_count_change, 1);
  EXPECT_EQ(RMW_QOS_POLICY_RELIABILITY, status.last_policy_kind);
  EXPECT_GE(callback_count.load(), 1U);

  EXPECT_EQ(RMW_RET_OK, rmw_event_fini(&event));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_wait_set(wait_set));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(node, publisher));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_subscription(node, subscription));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(node));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&options));
}
}  // namespace
