// Copyright 2026 Zenzen IoT
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ROSIDL_TYPESUPPORT_ZZDDS_CPP__MESSAGE_TYPE_SUPPORT_HPP_
#define ROSIDL_TYPESUPPORT_ZZDDS_CPP__MESSAGE_TYPE_SUPPORT_HPP_

#include <cstddef>
#include <cstdint>

#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_zzdds_cpp/visibility_control.h"
#include "zidl_cdr.h"  // NOLINT(build/include_subdir): installed at the zzdds include root
#include "zzdds_c.h"  // NOLINT(build/include_subdir): installed at the zzdds include root

namespace rosidl_typesupport_zzdds_cpp
{

/// Generated operations stored in rosidl_message_type_support_t::data.
struct message_type_support_callbacks_t
{
  const char * message_namespace;
  const char * message_name;
  const char * dds_type_name;

  /// Serialize a complete XCDR payload, including its encapsulation header.
  bool (* serialize)(const void * ros_message, ZidlCdrWriter * writer);

  /// Deserialize from an initialized reader into an existing ROS message.
  bool (* deserialize)(ZidlCdrReader * reader, void * ros_message);

  /// Allocate and initialize a ROS message for compatibility loaning.
  void * (* create_ros_message)();

  /// Finalize and deallocate a message returned from compatibility loaning.
  void (* destroy_ros_message)(void * ros_message);

  /// Return the complete serialized size, including encapsulation.
  /// SIZE_MAX indicates that serialization failed.
  size_t (* get_serialized_size)(const void * ros_message);

  /// Whether the IDL structure declares at least one @key member.
  bool has_key;

  /// Compute the DDS key hash. Null for unkeyed messages.
  bool (* compute_key_hash)(const void * ros_message, uint8_t hash[16]);

  /// Resolve a top-level scalar or string field from serialized CDR for CFT evaluation.
  zzdds_get_field_from_cdr_fn get_field_from_cdr;
};

ROSIDL_TYPESUPPORT_ZZDDS_CPP_PUBLIC
const rosidl_message_type_support_t *
get_message_typesupport_handle_function(
  const rosidl_message_type_support_t * handle,
  const char * identifier);

ROSIDL_TYPESUPPORT_ZZDDS_CPP_PUBLIC
const rosidl_service_type_support_t *
get_service_typesupport_handle_function(
  const rosidl_service_type_support_t * handle,
  const char * identifier);

template<typename T>
ROSIDL_TYPESUPPORT_ZZDDS_CPP_PUBLIC
const rosidl_message_type_support_t * get_message_type_support_handle();

}  // namespace rosidl_typesupport_zzdds_cpp

#endif  // ROSIDL_TYPESUPPORT_ZZDDS_CPP__MESSAGE_TYPE_SUPPORT_HPP_
