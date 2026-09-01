#ifndef RMW_ZZDDS_CPP__TYPE_SUPPORT_HPP_
#define RMW_ZZDDS_CPP__TYPE_SUPPORT_HPP_

#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_zzdds_cpp/message_type_support.hpp"

namespace rmw_zzdds_cpp
{

const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t *
resolve_message_type_support(const rosidl_message_type_support_t * type_support);

struct ServiceTypeSupport final
{
  const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t * request;
  const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t * response;
  rosidl_type_hash_t type_hash;
};

bool resolve_service_type_support(
  const rosidl_service_type_support_t * type_support,
  ServiceTypeSupport * result);

}  // namespace rmw_zzdds_cpp

#endif  // RMW_ZZDDS_CPP__TYPE_SUPPORT_HPP_
