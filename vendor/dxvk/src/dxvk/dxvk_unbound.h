#pragma once

#include <array>
#include <mutex>

#include "dxvk_buffer.h"
#include "dxvk_image.h"
#include "dxvk_sampler.h"

namespace dxvk {

  class DxvkContext;
  
  /**
   * \brief Unbound resources
   * 
   * Creates dummy resources that will be used
   * for descriptor sets when the client API did
   * not bind a compatible resource to a slot.
   */
  class DxvkUnboundResources {
    
  public:
    
    DxvkUnboundResources(DxvkDevice* dev);
    ~DxvkUnboundResources();
    
    /**
     * \brief Dummy buffer handle
     * 
     * Returns a handle to a buffer filled with zeroes.
     * Use for unbound transform feedback buffers only.
     * \returns Dummy buffer handle
     */
    DxvkResourceBufferInfo bufferInfo();

    /**
     * \brief Dummy sampler object
     * 
     * Points to a sampler which was created with
     * reasonable default values. Client APIs may
     * still require different behaviour.
     * \returns Dummy sampler
     */
    DxvkSamplerDescriptor samplerInfo();

    /**
     * \brief Dummy image view descriptor
     *
     * Returns a descriptor for a view of a small image with
     * zero-initialized contents. Used for unbound image bindings
     * on devices that do not support the nullDescriptor feature.
     * \param [in] type Image view type required by the binding
     * \param [in] storage Whether a storage image is required
     * \returns Dummy image view descriptor
     */
    const DxvkDescriptor* imageViewDescriptor(VkImageViewType type, bool storage);

    /**
     * \brief Dummy texel buffer view descriptor
     *
     * Returns a descriptor for a formatted view of the zero
     * buffer. Used for unbound texel buffer bindings on devices
     * that do not support the nullDescriptor feature.
     * \param [in] storage Whether a storage texel buffer is required
     * \returns Dummy buffer view descriptor
     */
    const DxvkDescriptor* bufferViewDescriptor(bool storage);

  private:

    DxvkDevice*             m_device;

    std::atomic<bool>       m_bufferCreated = { false };
    std::atomic<bool>       m_samplerCreated = { false };
    std::atomic<bool>       m_imagesCreated = { false };
    std::atomic<bool>       m_bufferViewsCreated = { false };

    dxvk::mutex             m_mutex;
    Rc<DxvkSampler>         m_sampler;
    Rc<DxvkBuffer>          m_buffer;

    Rc<DxvkImage>           m_image1D;
    Rc<DxvkImage>           m_image2D;
    Rc<DxvkImage>           m_image3D;

    std::array<Rc<DxvkImageView>, 2u * (VK_IMAGE_VIEW_TYPE_CUBE_ARRAY + 1u)> m_imageViews;

    Rc<DxvkBufferView>      m_bufferViewSampled;
    Rc<DxvkBufferView>      m_bufferViewStorage;

    Rc<DxvkSampler> createSampler();

    Rc<DxvkBuffer> createBuffer();

    Rc<DxvkImage> createImage(VkImageType type, VkImageCreateFlags flags, uint32_t layers);

    void createImages();

    void createBufferViews();

  };
  
}
