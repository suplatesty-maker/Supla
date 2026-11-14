#pragma once
#include <cstdint>
namespace Supla { namespace Html {
class CustomTextParameter {
 public:
  template <typename... A>
  explicit CustomTextParameter(A...) {}
  bool getParameterValue(char* out, int n) const {
    if (n > 0) out[0] = 0;
    return false;
  }
};
}}
