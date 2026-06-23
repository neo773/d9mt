// Single translation unit that emits metal-cpp's out-of-line symbols (selectors,
// class lookups). Exactly one TU in the build must define these macros before
// including the headers; every other file includes the headers normally.

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTLFX_PRIVATE_IMPLEMENTATION

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
