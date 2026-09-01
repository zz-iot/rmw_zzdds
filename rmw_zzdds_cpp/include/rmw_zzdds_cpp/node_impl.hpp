#ifndef RMW_ZZDDS_CPP__NODE_IMPL_HPP_
#define RMW_ZZDDS_CPP__NODE_IMPL_HPP_

#include "rmw/init.h"

namespace rmw_zzdds_cpp
{

struct NodeImpl final
{
  explicit NodeImpl(rmw_context_t * context_in) : context(context_in) {}
  rmw_context_t * context;
};

}  // namespace rmw_zzdds_cpp

#endif  // RMW_ZZDDS_CPP__NODE_IMPL_HPP_

