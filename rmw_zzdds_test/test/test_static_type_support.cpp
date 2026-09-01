#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#include <gtest/gtest.h>

#include "rcutils/allocator.h"
#include "rmw/enclave.h"
#include "rmw/error_handling.h"
#include "rmw/rmw.h"
#include "rmw_zzdds_test/msg/nested_record.hpp"
#include "rmw_zzdds_test/msg/primitive_record.hpp"
#include "rmw_zzdds_test/msg/keyed_record.hpp"
#include "rmw_zzdds_test/msg/detail/keyed_record__rosidl_typesupport_zzdds_cpp.hpp"
#include "rmw_zzdds_test/msg/detail/nested_record__rosidl_typesupport_zzdds_cpp.hpp"
#include "rmw_zzdds_test/msg/detail/primitive_record__rosidl_typesupport_zzdds_cpp.hpp"
#include "rosidl_typesupport_zzdds_cpp/identifier.hpp"
#include "rosidl_typesupport_zzdds_cpp/message_type_support.hpp"
#include "zidl_cdr.h"

namespace
{

rmw_zzdds_test::msg::PrimitiveRecord make_message()
{
  rmw_zzdds_test::msg::PrimitiveRecord message;
  message.flag = true;
  message.byte_value = 0xab;
  message.count = -2;
  message.id = 0x11223344u;
  message.big_value = 0x0102030405060708ll;
  message.ratio = 1.0f;
  message.measurement = -2.0;
  message.label = "Hi";
  message.samples = {0x0102u, 0x0304u, 0x0506u};
  message.values = {7, -8};
  return message;
}

const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t * callbacks()
{
  const auto * handle =
    rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
    rmw_zzdds_test::msg::PrimitiveRecord>();
  EXPECT_NE(nullptr, handle);
  EXPECT_STREQ(rosidl_typesupport_zzdds_cpp::typesupport_identifier,
      handle->typesupport_identifier);
  return static_cast<
    const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t *>(handle->data);
}

TEST(StaticTypeSupport, serializes_independent_xcdr1_golden_vector)
{
  const auto message = make_message();
  const auto * operations = callbacks();
  ASSERT_NE(nullptr, operations);
  EXPECT_STREQ("rmw_zzdds_test::msg::dds_::PrimitiveRecord_", operations->dds_type_name);

  ZidlCdrWriter writer{};
  ASSERT_EQ(ZIDL_CDR_OK, zidl_cdr_writer_init(&writer, ZIDL_XCDR1));
  ASSERT_TRUE(operations->serialize(&message, &writer));

  constexpr std::array<uint8_t, 64> expected = {
    0x00, 0x01, 0x00, 0x00,
    0x01, 0xab, 0xfe, 0xff,
    0x44, 0x33, 0x22, 0x11,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x00, 0x00, 0x80, 0x3f,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
    0x03, 0x00, 0x00, 0x00, 0x48, 0x69, 0x00, 0x00,
    0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x00, 0xf8, 0xff, 0xff, 0xff,
  };
  ASSERT_EQ(expected.size(), writer.len);
  EXPECT_EQ(0, std::memcmp(expected.data(), writer.buf, expected.size()));
  EXPECT_EQ(expected.size(), operations->get_serialized_size(&message));
  zidl_cdr_writer_deinit(&writer);
}

TEST(StaticTypeSupport, deserializes_golden_vector_and_nested_message)
{
  const auto original = make_message();
  rmw_zzdds_test::msg::NestedRecord nested;
  nested.record = original;
  nested.sequence = 42;
  nested.flags = {true, false, true};

  const auto * handle =
    rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
    rmw_zzdds_test::msg::NestedRecord>();
  ASSERT_NE(nullptr, handle);
  const auto * operations = static_cast<
    const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t *>(handle->data);
  ASSERT_NE(nullptr, operations);

  ZidlCdrWriter writer{};
  ASSERT_EQ(ZIDL_CDR_OK, zidl_cdr_writer_init(&writer, ZIDL_XCDR1));
  ASSERT_TRUE(operations->serialize(&nested, &writer));

  ZidlCdrReader reader{};
  ASSERT_EQ(ZIDL_CDR_OK, zidl_cdr_reader_init(&reader, writer.buf, writer.len));
  rmw_zzdds_test::msg::NestedRecord result;
  ASSERT_TRUE(operations->deserialize(&reader, &result));
  EXPECT_EQ(nested, result);
  EXPECT_EQ(0u, zidl_cdr_remaining(&reader));
  zidl_cdr_writer_deinit(&writer);
}

TEST(StaticTypeSupport, rejects_truncated_and_oversized_input)
{
  const auto original = make_message();
  const auto * operations = callbacks();
  ASSERT_NE(nullptr, operations);

  ZidlCdrWriter writer{};
  ASSERT_EQ(ZIDL_CDR_OK, zidl_cdr_writer_init(&writer, ZIDL_XCDR1));
  ASSERT_TRUE(operations->serialize(&original, &writer));

  ZidlCdrReader truncated{};
  ASSERT_EQ(ZIDL_CDR_OK, zidl_cdr_reader_init(&truncated, writer.buf, writer.len - 1));
  rmw_zzdds_test::msg::PrimitiveRecord result;
  EXPECT_FALSE(operations->deserialize(&truncated, &result));

  // Replace the bounded sequence length with five while only four are legal.
  std::array<uint8_t, 64> corrupt{};
  ASSERT_EQ(corrupt.size(), writer.len);
  std::memcpy(corrupt.data(), writer.buf, writer.len);
  corrupt[52] = 5;
  ZidlCdrReader oversized{};
  ASSERT_EQ(ZIDL_CDR_OK, zidl_cdr_reader_init(&oversized, corrupt.data(), corrupt.size()));
  EXPECT_FALSE(operations->deserialize(&oversized, &result));

  // A valid XCDR2 encapsulation must not be decoded using the XCDR1 layout.
  std::memcpy(corrupt.data(), writer.buf, writer.len);
  corrupt[1] = 0x07;
  ZidlCdrReader xcdr2{};
  ASSERT_EQ(ZIDL_CDR_OK, zidl_cdr_reader_init(&xcdr2, corrupt.data(), corrupt.size()));
  EXPECT_FALSE(operations->deserialize(&xcdr2, &result));
  zidl_cdr_writer_deinit(&writer);
}

TEST(StaticTypeSupport, computes_canonical_big_endian_key_hash)
{
  rmw_zzdds_test::msg::KeyedRecord message;
  message.key = 0x01020304;
  message.label = "not part of the key";

  const auto * handle =
    rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
    rmw_zzdds_test::msg::KeyedRecord>();
  ASSERT_NE(nullptr, handle);
  const auto * operations = static_cast<
    const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t *>(handle->data);
  ASSERT_NE(nullptr, operations);
  ASSERT_TRUE(operations->has_key);
  ASSERT_NE(nullptr, operations->compute_key_hash);

  std::array<uint8_t, 16> hash{};
  ASSERT_TRUE(operations->compute_key_hash(&message, hash.data()));
  constexpr std::array<uint8_t, 16> expected = {
    0x01, 0x02, 0x03, 0x04,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
  };
  EXPECT_EQ(expected, hash);
}

TEST(StaticTypeSupport, rmw_serialization_adapter_round_trips_and_rejects_bad_input)
{
  const auto original = make_message();
  const auto * handle =
    rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
    rmw_zzdds_test::msg::PrimitiveRecord>();
  ASSERT_NE(nullptr, handle);

  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rmw_serialized_message_t serialized = rmw_get_zero_initialized_serialized_message();
  ASSERT_EQ(RMW_RET_OK, rmw_serialized_message_init(&serialized, 0, &allocator));

  ASSERT_EQ(RMW_RET_OK, rmw_serialize(&original, handle, &serialized))
    << rmw_get_error_string().str;
  EXPECT_EQ(64u, serialized.buffer_length);

  rmw_zzdds_test::msg::PrimitiveRecord result;
  ASSERT_EQ(RMW_RET_OK, rmw_deserialize(&serialized, handle, &result))
    << rmw_get_error_string().str;
  EXPECT_EQ(original, result);

  const size_t full_length = serialized.buffer_length;
  serialized.buffer_length = full_length - 1;
  EXPECT_EQ(RMW_RET_ERROR, rmw_deserialize(&serialized, handle, &result));
  rmw_reset_error();
  serialized.buffer_length = full_length;

  rosidl_message_type_support_t foreign_handle = *handle;
  foreign_handle.typesupport_identifier = "foreign_typesupport";
  EXPECT_EQ(RMW_RET_ERROR, rmw_serialize(&original, &foreign_handle, &serialized));
  rmw_reset_error();

  EXPECT_EQ(RMW_RET_OK, rmw_serialized_message_fini(&serialized));
}

TEST(StaticTypeSupport, rmw_publisher_subscription_round_trip)
{
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rmw_init_options_t options = rmw_get_zero_initialized_init_options();
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&options, allocator));
  ASSERT_EQ(RMW_RET_OK, rmw_enclave_options_copy("/test", &allocator, &options.enclave));
  rmw_context_t context = rmw_get_zero_initialized_context();
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options, &context)) << rmw_get_error_string().str;
  rmw_node_t * node = rmw_create_node(&context, "static_typesupport_test", "/rmw_zzdds");
  ASSERT_NE(nullptr, node) << rmw_get_error_string().str;

  const auto * handle =
    rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
    rmw_zzdds_test::msg::PrimitiveRecord>();
  const rmw_subscription_options_t subscription_options =
    rmw_get_default_subscription_options();
  const rmw_publisher_options_t publisher_options = rmw_get_default_publisher_options();
  rmw_subscription_t * subscription = rmw_create_subscription(
    node, handle, "/static_round_trip", &rmw_qos_profile_default, &subscription_options);
  ASSERT_NE(nullptr, subscription) << rmw_get_error_string().str;
  rmw_publisher_t * publisher = rmw_create_publisher(
    node, handle, "/static_round_trip", &rmw_qos_profile_default, &publisher_options);
  ASSERT_NE(nullptr, publisher) << rmw_get_error_string().str;

  size_t matched = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    ASSERT_EQ(RMW_RET_OK, rmw_publisher_count_matched_subscriptions(publisher, &matched));
    if (matched > 0U) {break;}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_GT(matched, 0U);

  const auto input = make_message();
  ASSERT_EQ(RMW_RET_OK, rmw_publish(publisher, &input, nullptr))
    << rmw_get_error_string().str;
  rmw_wait_set_t * wait_set = rmw_create_wait_set(&context, 1U);
  ASSERT_NE(nullptr, wait_set) << rmw_get_error_string().str;
  void * subscription_entries[] = {subscription->data};
  rmw_subscriptions_t subscriptions{1U, subscription_entries};
  const rmw_time_t wait_timeout{10U, 0U};
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_wait(&subscriptions, nullptr, nullptr, nullptr, nullptr, wait_set, &wait_timeout))
    << rmw_get_error_string().str;
  EXPECT_EQ(subscription->data, subscription_entries[0]);
  rmw_zzdds_test::msg::PrimitiveRecord output;
  bool taken = false;
  while (std::chrono::steady_clock::now() < deadline) {
    ASSERT_EQ(RMW_RET_OK, rmw_take(subscription, &output, &taken, nullptr))
      << rmw_get_error_string().str;
    if (taken) {break;}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(taken);
  EXPECT_EQ(input, output);

  ASSERT_TRUE(publisher->can_loan_messages);
  ASSERT_TRUE(subscription->can_loan_messages);
  void * publisher_loan = nullptr;
  ASSERT_EQ(RMW_RET_OK, rmw_borrow_loaned_message(publisher, handle, &publisher_loan));
  ASSERT_NE(nullptr, publisher_loan);
  auto loaned_input = make_message();
  loaned_input.id = 0xa5a5a5a5U;
  *static_cast<rmw_zzdds_test::msg::PrimitiveRecord *>(publisher_loan) = loaned_input;
  ASSERT_EQ(RMW_RET_OK, rmw_publish_loaned_message(publisher, publisher_loan, nullptr));

  void * subscription_loan = nullptr;
  rmw_message_info_t loan_info = rmw_get_zero_initialized_message_info();
  taken = false;
  const auto loan_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < loan_deadline) {
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_take_loaned_message_with_info(
        subscription, &subscription_loan, &taken, &loan_info, nullptr))
      << rmw_get_error_string().str;
    if (taken) {break;}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(taken);
  ASSERT_NE(nullptr, subscription_loan);
  EXPECT_EQ(
    loaned_input,
    *static_cast<rmw_zzdds_test::msg::PrimitiveRecord *>(subscription_loan));
  rmw_gid_t publisher_gid{};
  ASSERT_EQ(RMW_RET_OK, rmw_get_gid_for_publisher(publisher, &publisher_gid));
  bool loan_gid_matches = false;
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_compare_gids_equal(&publisher_gid, &loan_info.publisher_gid, &loan_gid_matches));
  EXPECT_TRUE(loan_gid_matches);
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_return_loaned_message_from_subscription(subscription, subscription_loan));
  EXPECT_EQ(
    RMW_RET_ERROR,
    rmw_return_loaned_message_from_subscription(subscription, subscription_loan));
  rmw_reset_error();
  rmw_zzdds_test::msg::PrimitiveRecord foreign_message;
  EXPECT_EQ(
    RMW_RET_ERROR,
    rmw_return_loaned_message_from_publisher(publisher, &foreign_message));
  rmw_reset_error();

  rmw_serialized_message_t outbound = rmw_get_zero_initialized_serialized_message();
  rmw_serialized_message_t inbound = rmw_get_zero_initialized_serialized_message();
  ASSERT_EQ(RMW_RET_OK, rmw_serialized_message_init(&outbound, 0, &allocator));
  ASSERT_EQ(RMW_RET_OK, rmw_serialized_message_init(&inbound, 0, &allocator));
  ASSERT_EQ(RMW_RET_OK, rmw_serialize(&input, handle, &outbound));
  ASSERT_EQ(RMW_RET_OK, rmw_publish_serialized_message(publisher, &outbound, nullptr));
  taken = false;
  while (std::chrono::steady_clock::now() < deadline) {
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_take_serialized_message(subscription, &inbound, &taken, nullptr));
    if (taken) {break;}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(taken);
  EXPECT_EQ(outbound.buffer_length, inbound.buffer_length);
  EXPECT_EQ(0, std::memcmp(outbound.buffer, inbound.buffer, outbound.buffer_length));
  EXPECT_EQ(RMW_RET_OK, rmw_serialized_message_fini(&inbound));
  EXPECT_EQ(RMW_RET_OK, rmw_serialized_message_fini(&outbound));

  rmw_publisher_t * default_publisher = rmw_create_publisher(
    node, handle, "/system_defaults", &rmw_qos_profile_system_default, &publisher_options);
  ASSERT_NE(nullptr, default_publisher) << rmw_get_error_string().str;
  rmw_subscription_t * default_subscription = rmw_create_subscription(
    node, handle, "/system_defaults", &rmw_qos_profile_system_default,
    &subscription_options);
  ASSERT_NE(nullptr, default_subscription) << rmw_get_error_string().str;
  rmw_qos_profile_t publisher_qos = rmw_qos_profile_unknown;
  rmw_qos_profile_t subscription_qos = rmw_qos_profile_unknown;
  ASSERT_EQ(RMW_RET_OK, rmw_publisher_get_actual_qos(default_publisher, &publisher_qos));
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_subscription_get_actual_qos(default_subscription, &subscription_qos));
  EXPECT_EQ(RMW_QOS_POLICY_HISTORY_KEEP_LAST, publisher_qos.history);
  EXPECT_EQ(RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT, publisher_qos.reliability);
  EXPECT_EQ(RMW_QOS_POLICY_DURABILITY_VOLATILE, publisher_qos.durability);
  EXPECT_EQ(RMW_QOS_POLICY_LIVELINESS_AUTOMATIC, publisher_qos.liveliness);
  EXPECT_EQ(publisher_qos.history, subscription_qos.history);
  EXPECT_EQ(publisher_qos.reliability, subscription_qos.reliability);
  EXPECT_EQ(publisher_qos.durability, subscription_qos.durability);
  EXPECT_EQ(publisher_qos.liveliness, subscription_qos.liveliness);
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_subscription(node, default_subscription));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(node, default_publisher));

  EXPECT_EQ(RMW_RET_OK, rmw_destroy_wait_set(wait_set));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(node, publisher));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_subscription(node, subscription));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(node));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&options));
}

TEST(StaticTypeSupport, ignore_local_skips_to_remote_sample_and_preserves_gid)
{
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rmw_init_options_t local_options = rmw_get_zero_initialized_init_options();
  rmw_init_options_t remote_options = rmw_get_zero_initialized_init_options();
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&local_options, allocator));
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&remote_options, allocator));
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_enclave_options_copy("/local", &allocator, &local_options.enclave));
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_enclave_options_copy("/remote", &allocator, &remote_options.enclave));
  rmw_context_t local_context = rmw_get_zero_initialized_context();
  rmw_context_t remote_context = rmw_get_zero_initialized_context();
  ASSERT_EQ(RMW_RET_OK, rmw_init(&local_options, &local_context));
  ASSERT_EQ(RMW_RET_OK, rmw_init(&remote_options, &remote_context));
  rmw_node_t * local_node = rmw_create_node(&local_context, "local_node", "/rmw_zzdds");
  rmw_node_t * remote_node = rmw_create_node(&remote_context, "remote_node", "/rmw_zzdds");
  ASSERT_NE(nullptr, local_node);
  ASSERT_NE(nullptr, remote_node);

  const auto * handle =
    rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
    rmw_zzdds_test::msg::PrimitiveRecord>();
  ASSERT_NE(nullptr, handle);
  rmw_subscription_options_t subscription_options = rmw_get_default_subscription_options();
  subscription_options.ignore_local_publications = true;
  const rmw_publisher_options_t publisher_options = rmw_get_default_publisher_options();
  rmw_subscription_t * subscription = rmw_create_subscription(
    local_node, handle, "/ignore_local_mixed", &rmw_qos_profile_default,
    &subscription_options);
  rmw_publisher_t * local_publisher = rmw_create_publisher(
    local_node, handle, "/ignore_local_mixed", &rmw_qos_profile_default,
    &publisher_options);
  rmw_publisher_t * remote_publisher = rmw_create_publisher(
    remote_node, handle, "/ignore_local_mixed", &rmw_qos_profile_default,
    &publisher_options);
  ASSERT_NE(nullptr, subscription);
  ASSERT_NE(nullptr, local_publisher);
  ASSERT_NE(nullptr, remote_publisher);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  size_t local_matches = 0U;
  size_t remote_matches = 0U;
  while (std::chrono::steady_clock::now() < deadline) {
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_publisher_count_matched_subscriptions(local_publisher, &local_matches));
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_publisher_count_matched_subscriptions(remote_publisher, &remote_matches));
    if (local_matches != 0U && remote_matches != 0U) {break;}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_NE(0U, local_matches);
  ASSERT_NE(0U, remote_matches);

  auto local_message = make_message();
  auto remote_message = make_message();
  local_message.id = 1U;
  remote_message.id = 2U;
  ASSERT_EQ(RMW_RET_OK, rmw_publish(local_publisher, &local_message, nullptr));
  ASSERT_EQ(RMW_RET_OK, rmw_publish(remote_publisher, &remote_message, nullptr));

  rmw_zzdds_test::msg::PrimitiveRecord output;
  rmw_message_info_t info = rmw_get_zero_initialized_message_info();
  bool taken = false;
  while (std::chrono::steady_clock::now() < deadline) {
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_take_with_info(subscription, &output, &taken, &info, nullptr))
      << rmw_get_error_string().str;
    if (taken) {break;}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(taken);
  EXPECT_EQ(remote_message, output);
  rmw_gid_t remote_gid{};
  ASSERT_EQ(RMW_RET_OK, rmw_get_gid_for_publisher(remote_publisher, &remote_gid));
  bool gids_equal = false;
  ASSERT_EQ(RMW_RET_OK, rmw_compare_gids_equal(&remote_gid, &info.publisher_gid, &gids_equal));
  EXPECT_TRUE(gids_equal);

  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(remote_node, remote_publisher));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(local_node, local_publisher));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_subscription(local_node, subscription));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(remote_node));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(local_node));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&remote_context));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&local_context));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&remote_context));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&local_context));
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&remote_options));
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&local_options));
}

}  // namespace
