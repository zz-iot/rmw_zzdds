#ifndef RMW_ZZDDS_CPP__GUARD_CONDITION_IMPL_HPP_
#define RMW_ZZDDS_CPP__GUARD_CONDITION_IMPL_HPP_

#include "rmw/init.h"

#include "dcps.h"

namespace rmw_zzdds_cpp
{

struct GuardConditionImpl final
{
  GuardConditionImpl(rmw_context_t * context_in, DDS_GuardCondition native_in)
  : context(context_in), native(native_in) {}

  rmw_context_t * context;
  DDS_GuardCondition native;
};

}  // namespace rmw_zzdds_cpp

#endif  // RMW_ZZDDS_CPP__GUARD_CONDITION_IMPL_HPP_

