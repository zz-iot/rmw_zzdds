#include "rmw_zzdds_cpp/type_support.hpp"

#include <cstring>

#include "rmw/error_handling.h"
#include "rosidl_typesupport_zzdds_c/identifier.hpp"
#include "rosidl_typesupport_zzdds_cpp/identifier.hpp"

namespace rmw_zzdds_cpp
{

const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t *
resolve_message_type_support(const rosidl_message_type_support_t * type_support)
{
  if (type_support == nullptr || type_support->typesupport_identifier == nullptr) {
    RMW_SET_ERROR_MSG("a valid message type support handle is required");
    return nullptr;
  }
  const rosidl_message_type_support_t * resolved = nullptr;
  if (std::strcmp(type_support->typesupport_identifier,
      rosidl_typesupport_zzdds_cpp::typesupport_identifier) == 0 ||
    std::strcmp(type_support->typesupport_identifier,
      rosidl_typesupport_zzdds_c::typesupport_identifier) == 0)
  {
    resolved = type_support;
  } else if (type_support->func != nullptr) {
    const bool is_c_dispatch =
      std::strcmp(type_support->typesupport_identifier, "rosidl_typesupport_c") == 0;
    const char * first_identifier = is_c_dispatch ?
      rosidl_typesupport_zzdds_c::typesupport_identifier :
      rosidl_typesupport_zzdds_cpp::typesupport_identifier;
    const char * second_identifier = is_c_dispatch ?
      rosidl_typesupport_zzdds_cpp::typesupport_identifier :
      rosidl_typesupport_zzdds_c::typesupport_identifier;
    resolved = type_support->func(type_support, first_identifier);
    if (resolved == nullptr) {
      rmw_reset_error();
      resolved = type_support->func(type_support, second_identifier);
    }
  }
  if (resolved == nullptr || resolved->typesupport_identifier == nullptr ||
    (std::strcmp(resolved->typesupport_identifier,
      rosidl_typesupport_zzdds_cpp::typesupport_identifier) != 0 &&
    std::strcmp(resolved->typesupport_identifier,
      rosidl_typesupport_zzdds_c::typesupport_identifier) != 0) ||
    resolved->data == nullptr)
  {
    rmw_reset_error();
    RMW_SET_ERROR_MSG("message type support does not provide a zzdds static type support");
    return nullptr;
  }
  const auto * callbacks = static_cast<
    const rosidl_typesupport_zzdds_cpp::message_type_support_callbacks_t *>(resolved->data);
  if (callbacks->serialize == nullptr || callbacks->deserialize == nullptr ||
    callbacks->get_serialized_size == nullptr || callbacks->dds_type_name == nullptr)
  {
    RMW_SET_ERROR_MSG("zzdds static type support callback table is incomplete");
    return nullptr;
  }
  return callbacks;
}

bool resolve_service_type_support(
  const rosidl_service_type_support_t * type_support,
  ServiceTypeSupport * result)
{
  if (type_support == nullptr || result == nullptr ||
    type_support->typesupport_identifier == nullptr)
  {
    RMW_SET_ERROR_MSG("a valid service type support handle is required");
    return false;
  }
  const rosidl_service_type_support_t * resolved = type_support;
  const bool is_zzdds_cpp = std::strcmp(type_support->typesupport_identifier,
    rosidl_typesupport_zzdds_cpp::typesupport_identifier) == 0;
  const bool is_zzdds_c = std::strcmp(type_support->typesupport_identifier,
    rosidl_typesupport_zzdds_c::typesupport_identifier) == 0;
  if (!is_zzdds_cpp && !is_zzdds_c)
  {
    const bool is_c_dispatch = std::strcmp(
      type_support->typesupport_identifier, "rosidl_typesupport_c") == 0;
    const char * first = is_c_dispatch ? rosidl_typesupport_zzdds_c::typesupport_identifier :
      rosidl_typesupport_zzdds_cpp::typesupport_identifier;
    const char * second = is_c_dispatch ? rosidl_typesupport_zzdds_cpp::typesupport_identifier :
      rosidl_typesupport_zzdds_c::typesupport_identifier;
    resolved = type_support->func == nullptr ? nullptr : type_support->func(type_support, first);
    if (resolved == nullptr && type_support->func != nullptr) {
      rmw_reset_error();
      resolved = type_support->func(type_support, second);
    }
  }
  if (resolved == nullptr || resolved->request_typesupport == nullptr ||
    resolved->response_typesupport == nullptr)
  {
    rmw_reset_error();
    RMW_SET_ERROR_MSG("service type support does not provide zzdds request and response support");
    return false;
  }
  const auto * request = resolve_message_type_support(resolved->request_typesupport);
  if (request == nullptr) {
    return false;
  }
  const auto * response = resolve_message_type_support(resolved->response_typesupport);
  if (response == nullptr) {
    return false;
  }
  rosidl_type_hash_t type_hash = rosidl_get_zero_initialized_type_hash();
  if (resolved->get_type_hash_func != nullptr) {
    const rosidl_type_hash_t * resolved_hash = resolved->get_type_hash_func(resolved);
    if (resolved_hash != nullptr) {type_hash = *resolved_hash;}
  }
  *result = {request, response, type_hash};
  return true;
}

}  // namespace rmw_zzdds_cpp
