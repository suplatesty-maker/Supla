#pragma once
#include <cstdint>
namespace Supla { namespace Html {
template <typename T>
class CustomParameterTemplate {
 public:
  template <typename... A>
  explicit CustomParameterTemplate(A...) {}
  T getParameterValue() { return T(); }
};
}}
