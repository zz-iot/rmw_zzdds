#include "rmw/rmw.h"

#include "rmw_zzdds_cpp/identifier.hpp"

namespace rmw_zzdds_cpp
{

const char identifier[] = "rmw_zzdds_cpp";
const char serialization_format[] = "cdr";

}  // namespace rmw_zzdds_cpp

extern "C"
{

const char * rmw_get_implementation_identifier()
{
  return rmw_zzdds_cpp::identifier;
}

const char * rmw_get_serialization_format()
{
  return rmw_zzdds_cpp::serialization_format;
}

}  // extern "C"

