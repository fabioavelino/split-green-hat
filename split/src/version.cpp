#include "split/version.hpp"

#ifndef SPLIT_VERSION
#define SPLIT_VERSION "dev"
#endif

namespace split {

const char* Version() {
  return SPLIT_VERSION;
}

}  // namespace split
