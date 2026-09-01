# Copyright 2026 Zenzen IoT
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if(NOT TARGET ${rosidl_generate_interfaces_TARGET}__rosidl_generator_c)
  message(FATAL_ERROR "rosidl_generator_c must run before rosidl_typesupport_zzdds_c")
endif()

find_package(rmw REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_interface REQUIRED)
find_package(rosidl_typesupport_zzdds_cpp REQUIRED)
find_package(ZZDDS CONFIG REQUIRED)

set(_output_path
  "${CMAKE_CURRENT_BINARY_DIR}/rosidl_typesupport_zzdds_c/${PROJECT_NAME}")
set(_generated_files "")
foreach(_abs_idl_file ${rosidl_generate_interfaces_ABS_IDL_FILES})
  get_filename_component(_parent_folder "${_abs_idl_file}" DIRECTORY)
  get_filename_component(_parent_folder "${_parent_folder}" NAME)
  get_filename_component(_idl_name "${_abs_idl_file}" NAME_WE)
  string_camel_case_to_lower_case_underscore("${_idl_name}" _header_name)
  list(APPEND _generated_files
    "${_output_path}/${_parent_folder}/detail/dds_zzdds_c/${_header_name}__type_support.cpp"
    "${_output_path}/${_parent_folder}/detail/${_header_name}__rosidl_typesupport_zzdds_c.h")
endforeach()

set(_dependency_files "")
set(_dependencies "")
foreach(_pkg_name ${rosidl_generate_interfaces_DEPENDENCY_PACKAGE_NAMES})
  foreach(_idl_file ${${_pkg_name}_IDL_FILES})
    set(_abs_idl_file "${${_pkg_name}_DIR}/../${_idl_file}")
    normalize_path(_abs_idl_file "${_abs_idl_file}")
    list(APPEND _dependency_files "${_abs_idl_file}")
    list(APPEND _dependencies "${_pkg_name}:${_abs_idl_file}")
  endforeach()
endforeach()

set(_templates
  "${rosidl_typesupport_zzdds_c_TEMPLATE_DIR}/idl__type_support_c.cpp.em"
  "${rosidl_typesupport_zzdds_c_TEMPLATE_DIR}/idl__rosidl_typesupport_zzdds_c.h.em")
set(_arguments_file
  "${CMAKE_CURRENT_BINARY_DIR}/rosidl_typesupport_zzdds_c__arguments.json")
rosidl_write_generator_arguments(
  "${_arguments_file}"
  PACKAGE_NAME "${PROJECT_NAME}"
  IDL_TUPLES "${rosidl_generate_interfaces_IDL_TUPLES}"
  ROS_INTERFACE_DEPENDENCIES "${_dependencies}"
  OUTPUT_DIR "${_output_path}"
  TEMPLATE_DIR "${rosidl_typesupport_zzdds_c_TEMPLATE_DIR}"
  TARGET_DEPENDENCIES ${rosidl_typesupport_zzdds_c_BIN} ${_templates}
    ${rosidl_generate_interfaces_ABS_IDL_FILES} ${_dependency_files})

add_custom_command(
  OUTPUT ${_generated_files}
  COMMAND "${rosidl_typesupport_zzdds_c_BIN}"
    --generator-arguments-file "${_arguments_file}"
  DEPENDS ${rosidl_typesupport_zzdds_c_BIN} ${_templates}
    ${rosidl_generate_interfaces_ABS_IDL_FILES} ${_dependency_files}
  COMMENT "Generating ROS C-layout type support for zzdds"
  VERBATIM)

set(_target_suffix "__rosidl_typesupport_zzdds_c")
add_library(${rosidl_generate_interfaces_TARGET}${_target_suffix}
  ${rosidl_typesupport_zzdds_c_LIBRARY_TYPE} ${_generated_files})
set_target_properties(${rosidl_generate_interfaces_TARGET}${_target_suffix}
  PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
target_compile_options(${rosidl_generate_interfaces_TARGET}${_target_suffix}
  PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
target_include_directories(${rosidl_generate_interfaces_TARGET}${_target_suffix} PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/rosidl_typesupport_zzdds_c>"
  "$<INSTALL_INTERFACE:include/${PROJECT_NAME}>")
target_link_libraries(${rosidl_generate_interfaces_TARGET}${_target_suffix} PUBLIC
  ZZDDS::zzdds rmw::rmw rosidl_runtime_c::rosidl_runtime_c
  rosidl_typesupport_interface::rosidl_typesupport_interface
  rosidl_typesupport_zzdds_c::rosidl_typesupport_zzdds_c
  rosidl_typesupport_zzdds_cpp::rosidl_typesupport_zzdds_cpp
  ${rosidl_generate_interfaces_TARGET}__rosidl_generator_c)
foreach(_pkg_name ${rosidl_generate_interfaces_DEPENDENCY_PACKAGE_NAMES})
  target_link_libraries(${rosidl_generate_interfaces_TARGET}${_target_suffix} PUBLIC
    ${${_pkg_name}_TARGETS${_target_suffix}})
endforeach()
add_dependencies(${rosidl_generate_interfaces_TARGET}
  ${rosidl_generate_interfaces_TARGET}${_target_suffix})

if(NOT rosidl_generate_interfaces_SKIP_INSTALL)
  install(DIRECTORY "${_output_path}/" DESTINATION "include/${PROJECT_NAME}/${PROJECT_NAME}"
    PATTERN "*.cpp" EXCLUDE)
  rosidl_export_typesupport_libraries(
    ${_target_suffix} ${rosidl_generate_interfaces_TARGET}${_target_suffix})
  ament_export_targets(export_${rosidl_generate_interfaces_TARGET}${_target_suffix})
  rosidl_export_typesupport_targets(
    ${_target_suffix} ${rosidl_generate_interfaces_TARGET}${_target_suffix})
  install(TARGETS ${rosidl_generate_interfaces_TARGET}${_target_suffix}
    EXPORT export_${rosidl_generate_interfaces_TARGET}${_target_suffix}
    ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
  ament_export_dependencies(rmw rosidl_runtime_c rosidl_typesupport_interface
    rosidl_typesupport_zzdds_c rosidl_typesupport_zzdds_cpp ZZDDS)
endif()
