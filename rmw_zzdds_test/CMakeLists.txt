cmake_minimum_required(VERSION 3.20)
project(rmw_zzdds_test VERSION 0.1.0 LANGUAGES C CXX)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(rosidl_typesupport_zzdds_cpp REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/KeyedRecord.idl"
  "msg/PrimitiveRecord.msg"
  "msg/NestedRecord.msg"
  "srv/RoundTrip.srv")

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(rmw_zzdds_cpp REQUIRED)
  find_package(ZZDDS CONFIG REQUIRED)

  rosidl_get_typesupport_target(
    zzdds_typesupport_target ${PROJECT_NAME} rosidl_typesupport_zzdds_cpp)
  rosidl_get_typesupport_target(
    cpp_typesupport_target ${PROJECT_NAME} rosidl_typesupport_cpp)
  ament_add_gtest(test_static_type_support test/test_static_type_support.cpp)
  if(TARGET test_static_type_support)
    target_compile_features(test_static_type_support PRIVATE cxx_std_17)
    target_link_libraries(test_static_type_support
      ${zzdds_typesupport_target}
      rmw_zzdds_cpp::rmw_zzdds_cpp
      ZZDDS::zzdds)
  endif()

  ament_add_gtest(test_service_round_trip test/test_service_round_trip.cpp)
  if(TARGET test_service_round_trip)
    target_compile_features(test_service_round_trip PRIVATE cxx_std_17)
    target_link_libraries(test_service_round_trip
      ${cpp_typesupport_target}
      ${zzdds_typesupport_target}
      rmw_zzdds_cpp::rmw_zzdds_cpp
      ZZDDS::zzdds)
  endif()

  ament_add_gtest(test_incompatible_qos_event test/test_incompatible_qos_event.cpp)
  if(TARGET test_incompatible_qos_event)
    target_compile_features(test_incompatible_qos_event PRIVATE cxx_std_17)
    target_link_libraries(test_incompatible_qos_event
      ${zzdds_typesupport_target}
      rmw_zzdds_cpp::rmw_zzdds_cpp
      ZZDDS::zzdds)
  endif()

  ament_add_gtest(test_local_graph test/test_local_graph.cpp)
  if(TARGET test_local_graph)
    target_compile_features(test_local_graph PRIVATE cxx_std_17)
    target_link_libraries(test_local_graph
      ${cpp_typesupport_target}
      ${zzdds_typesupport_target}
      rmw_zzdds_cpp::rmw_zzdds_cpp
      ZZDDS::zzdds)
  endif()
endif()

ament_export_dependencies(rosidl_default_runtime)
ament_package()
