#pragma once

// DxvkExtensionProvider shim — pure interface, no Vulkan runtime.
//
// The instance header includes this for the DxvkExtensionList typedef and the
// abstract provider interface (used for OpenVR/OpenXR extension injection in
// real DXVK). The shim never registers a provider, but the type must exist so
// the instance/adapter headers compile. Kept verbatim from dxvk-ref since it
// contains only declarations — VkExtensionProperties is a plain Vulkan struct.

#include "dxvk_include.h"

#include <string>
#include <string_view>
#include <vector>

namespace dxvk {

  class DxvkInstance;
  class DxvkExtensionProvider;

  using DxvkExtensionList = std::vector<VkExtensionProperties>;

  // Abstract extension provider. No shim implementation is registered; this
  // exists only so the instance/adapter types that reference it compile.
  class DxvkExtensionProvider {

  public:

    virtual std::string_view getName() = 0;

    virtual DxvkExtensionList getInstanceExtensions() = 0;

    virtual DxvkExtensionList getDeviceExtensions(
            uint32_t      adapterId) = 0;

    virtual void initInstanceExtensions() = 0;

    virtual void initDeviceExtensions(
      const DxvkInstance* instance) = 0;

  };

}
