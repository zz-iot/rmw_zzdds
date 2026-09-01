#include <array>
#include <cstring>

#include <gtest/gtest.h>

#include "dcps.h"
#include "zzdds_c.h"

namespace
{

bool wait_for_status(DDS_Entity entity, DDS_StatusMask mask)
{
  DDS_StatusCondition status = DDS_Entity_get_statuscondition(entity);
  if (status == nullptr ||
    DDS_StatusCondition_set_enabled_statuses(status, mask) != DDS_RETCODE_OK)
  {
    return false;
  }
  DDS_WaitSet waitset = zzdds_create_waitset();
  if (zzdds_waitset_is_nil(waitset)) {
    return false;
  }
  DDS_Condition condition = DDS_StatusCondition_as_DDS_Condition(status);
  if (DDS_WaitSet_attach_condition(waitset, condition) != DDS_RETCODE_OK) {
    zzdds_destroy_waitset(waitset);
    return false;
  }
  DDS_ConditionSeq active{};
  const DDS_Duration_t timeout{10, 0U};
  const bool triggered = DDS_WaitSet_wait(waitset, &active, &timeout) == DDS_RETCODE_OK;
  DDS_ConditionSeq_free(&active);
  (void)DDS_WaitSet_detach_condition(waitset, condition);
  zzdds_destroy_waitset(waitset);
  return triggered;
}

}  // namespace

TEST(ZzddsCBinding, ParticipantContainersAndGuardConditionLifecycle)
{
  zzdds_DomainParticipantFactory factory = zzdds_create_factory();
  ASSERT_FALSE(zzdds_factory_is_nil(factory));

  DDS_DomainParticipantFactory dds_factory =
    zzdds_DomainParticipantFactory_as_DDS_DomainParticipantFactory(factory);
  ASSERT_NE(nullptr, dds_factory);

  DDS_DomainParticipant participant = DDS_DomainParticipantFactory_create_participant(
    dds_factory, 0U, nullptr, nullptr, 0U);
  ASSERT_NE(nullptr, participant);
  DDS_Publisher publisher = DDS_DomainParticipant_create_publisher(
    participant, nullptr, nullptr, 0U);
  ASSERT_NE(nullptr, publisher);
  DDS_Subscriber subscriber = DDS_DomainParticipant_create_subscriber(
    participant, nullptr, nullptr, 0U);
  ASSERT_NE(nullptr, subscriber);

  DDS_GuardCondition guard = zzdds_create_guardcondition();
  ASSERT_FALSE(zzdds_guardcondition_is_nil(guard));
  EXPECT_EQ(DDS_RETCODE_OK, DDS_GuardCondition_set_trigger_value(guard, true));
  EXPECT_TRUE(DDS_GuardCondition_get_trigger_value(guard));
  zzdds_destroy_guardcondition(guard);

  EXPECT_EQ(DDS_RETCODE_OK, DDS_DomainParticipant_delete_subscriber(participant, subscriber));
  EXPECT_EQ(DDS_RETCODE_OK, DDS_DomainParticipant_delete_publisher(participant, publisher));
  EXPECT_EQ(DDS_RETCODE_OK, DDS_DomainParticipantFactory_delete_participant(
      dds_factory, participant));
  zzdds_destroy_factory(factory);
}

TEST(ZzddsCBinding, SerializedWriterReaderRoundTrip)
{
  zzdds_DomainParticipantFactory factory = zzdds_create_factory();
  ASSERT_FALSE(zzdds_factory_is_nil(factory));
  DDS_DomainParticipantFactory dds_factory =
    zzdds_DomainParticipantFactory_as_DDS_DomainParticipantFactory(factory);
  ASSERT_NE(nullptr, dds_factory);

  DDS_DomainParticipant participant = DDS_DomainParticipantFactory_create_participant(
    dds_factory, 0U, nullptr, nullptr, DDS_STATUS_MASK_NONE);
  ASSERT_NE(nullptr, participant);
  ASSERT_EQ(0, zzdds_register_type_support(
      participant, "rmw_zzdds_test_type", nullptr, nullptr));

  DDS_Publisher publisher = DDS_DomainParticipant_create_publisher(
    participant, nullptr, nullptr, DDS_STATUS_MASK_NONE);
  DDS_Subscriber subscriber = DDS_DomainParticipant_create_subscriber(
    participant, nullptr, nullptr, DDS_STATUS_MASK_NONE);
  ASSERT_NE(nullptr, publisher);
  ASSERT_NE(nullptr, subscriber);

  DDS_Topic topic = DDS_DomainParticipant_create_topic(
    participant, "rt/rmw_zzdds_binding_test", "rmw_zzdds_test_type",
    nullptr, nullptr, DDS_STATUS_MASK_NONE);
  ASSERT_NE(nullptr, topic);
  DDS_DataWriter writer = DDS_Publisher_create_datawriter(
    publisher, topic, nullptr, nullptr, DDS_STATUS_MASK_NONE);
  DDS_DataReader reader = DDS_Subscriber_create_datareader(
    subscriber, zzdds_topic_as_description(topic), nullptr, nullptr, DDS_STATUS_MASK_NONE);
  ASSERT_NE(nullptr, writer);
  ASSERT_NE(nullptr, reader);
  ASSERT_TRUE(wait_for_status(
      DDS_DataWriter_as_DDS_Entity(writer), DDS_PUBLICATION_MATCHED_STATUS));

  constexpr std::array<uint8_t, 12> payload = {
    0x00, 0x01, 0x00, 0x00,
    0x44, 0x33, 0x22, 0x11,
    0x88, 0x77, 0x66, 0x55,
  };
  constexpr std::array<uint8_t, 16> key_hash{};
  DDS_OctetSeq dds_key{
    key_hash.size(), key_hash.size(), const_cast<uint8_t *>(key_hash.data()), false};
  DDS_OctetSeq dds_payload{
    payload.size(), payload.size(), const_cast<uint8_t *>(payload.data()), false};
  const DDS_Time_t source_timestamp{DDS_TIME_INVALID_SEC, DDS_TIME_INVALID_NSEC};
  ASSERT_EQ(
    DDS_RETCODE_OK,
    DDS_DataWriter_write_raw(
      writer, &dds_key, DDS_HANDLE_NIL, &dds_payload,
      DDS_WriteKind_ALIVE_WRITE_KIND, &source_timestamp));
  ASSERT_TRUE(wait_for_status(
      DDS_DataReader_as_DDS_Entity(reader), DDS_DATA_AVAILABLE_STATUS));

  DDS_OctetSeqSeq loaned_payloads{};
  DDS_OctetSeq loaned_hashes{};
  DDS_SampleInfoSeq loaned_infos{};
  ASSERT_EQ(
    DDS_RETCODE_OK,
    DDS_DataReader_take_raw(
      reader, &loaned_payloads, &loaned_hashes, &loaned_infos, DDS_HANDLE_NIL,
      nullptr, DDS_ANY_SAMPLE_STATE, DDS_ANY_VIEW_STATE, DDS_ANY_INSTANCE_STATE, 1));
  ASSERT_EQ(1U, loaned_payloads._length);
  ASSERT_EQ(1U, loaned_infos._length);
  ASSERT_TRUE(loaned_infos._buffer[0].valid_data);
  ASSERT_EQ(payload.size(), loaned_payloads._buffer[0]._length);
  EXPECT_EQ(
    0,
    std::memcmp(payload.data(), loaned_payloads._buffer[0]._buffer, payload.size()));
  EXPECT_EQ(
    DDS_RETCODE_OK,
    DDS_DataReader_return_loan_raw(
      reader, &loaned_payloads, &loaned_hashes, &loaned_infos));

  EXPECT_EQ(DDS_RETCODE_OK, DDS_Subscriber_delete_datareader(subscriber, reader));
  EXPECT_EQ(DDS_RETCODE_OK, DDS_Publisher_delete_datawriter(publisher, writer));
  EXPECT_EQ(DDS_RETCODE_OK, DDS_DomainParticipant_delete_topic(participant, topic));
  EXPECT_EQ(DDS_RETCODE_OK, DDS_DomainParticipant_delete_subscriber(participant, subscriber));
  EXPECT_EQ(DDS_RETCODE_OK, DDS_DomainParticipant_delete_publisher(participant, publisher));
  EXPECT_EQ(DDS_RETCODE_OK, DDS_DomainParticipantFactory_delete_participant(
      dds_factory, participant));
  zzdds_destroy_factory(factory);
}
