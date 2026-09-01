#include <limits>

#include "rcutils/allocator.h"
#include "rcutils/types/rcutils_ret.h"
#include "rcutils/types/uint8_array.h"
#include "rmw/error_handling.h"
#include "rmw/rmw.h"
#include "rmw_zzdds_cpp/type_support.hpp"
#include "zidl_cdr.h"

namespace
{

bool valid_serialized_message(const rmw_serialized_message_t * message)
{
  return message != nullptr && rcutils_allocator_is_valid(&message->allocator) &&
         message->buffer_length <= message->buffer_capacity &&
         (message->buffer_capacity == 0 || message->buffer != nullptr);
}

}  // namespace

extern "C"
{

rmw_ret_t rmw_serialize(
  const void * ros_message,
  const rosidl_message_type_support_t * type_support,
  rmw_serialized_message_t * serialized_message)
{
  if (ros_message == nullptr) {
    RMW_SET_ERROR_MSG("a ROS message is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (!valid_serialized_message(serialized_message)) {
    RMW_SET_ERROR_MSG("an initialized serialized message is required");
    return RMW_RET_INVALID_ARGUMENT;
  }

  const auto * callbacks = rmw_zzdds_cpp::resolve_message_type_support(type_support);
  if (callbacks == nullptr) {
    return RMW_RET_ERROR;
  }

  const size_t required_size = callbacks->get_serialized_size(ros_message);
  if (required_size == std::numeric_limits<size_t>::max() || required_size < 4) {
    RMW_SET_ERROR_MSG("failed to determine the serialized message size");
    return RMW_RET_ERROR;
  }

  const size_t previous_length = serialized_message->buffer_length;
  if (serialized_message->buffer_capacity < required_size) {
    const rcutils_ret_t resize_result =
      rcutils_uint8_array_resize(serialized_message, required_size);
    if (resize_result != RCUTILS_RET_OK) {
      serialized_message->buffer_length = previous_length;
      if (resize_result == RCUTILS_RET_BAD_ALLOC) {
        return RMW_RET_BAD_ALLOC;
      }
      return RMW_RET_ERROR;
    }
  }

  ZidlCdrWriter writer{};
  zidl_cdr_writer_init_fixed(
    &writer, serialized_message->buffer, serialized_message->buffer_capacity, ZIDL_XCDR1);
  if (!callbacks->serialize(ros_message, &writer) || writer.len != required_size) {
    serialized_message->buffer_length = previous_length;
    RMW_SET_ERROR_MSG("failed to serialize the ROS message as XCDR1");
    return RMW_RET_ERROR;
  }

  serialized_message->buffer_length = writer.len;
  return RMW_RET_OK;
}

rmw_ret_t rmw_deserialize(
  const rmw_serialized_message_t * serialized_message,
  const rosidl_message_type_support_t * type_support,
  void * ros_message)
{
  if (ros_message == nullptr) {
    RMW_SET_ERROR_MSG("a ROS message is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (!valid_serialized_message(serialized_message) || serialized_message->buffer_length < 4) {
    RMW_SET_ERROR_MSG("an initialized serialized message containing CDR data is required");
    return RMW_RET_INVALID_ARGUMENT;
  }

  const auto * callbacks = rmw_zzdds_cpp::resolve_message_type_support(type_support);
  if (callbacks == nullptr) {
    return RMW_RET_ERROR;
  }

  ZidlCdrReader reader{};
  if (
    zidl_cdr_reader_init(
      &reader, serialized_message->buffer, serialized_message->buffer_length) != ZIDL_CDR_OK ||
    !callbacks->deserialize(&reader, ros_message))
  {
    RMW_SET_ERROR_MSG("failed to deserialize the XCDR message");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

}  // extern "C"
