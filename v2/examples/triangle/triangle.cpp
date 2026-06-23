// triangle — first v2 Metal milestone: a white triangle on a black background,
// rendered fully offscreen via metal-cpp (no Objective-C, no windowing yet) and
// written to a BMP. Validates the Metal backend end-to-end: device + queue,
// runtime MSL compile, render pipeline state, a render pass with a black clear,
// and a single 3-vertex draw. Windowing/present is a later milestone.

#include <cstdint>
#include <cstdio>
#include <vector>

#include <Metal/Metal.hpp>

#include "metal_device.h"

namespace {

  constexpr uint32_t kWidth  = 512;
  constexpr uint32_t kHeight = 512;

  // Positions are generated from the vertex id, so the draw needs no vertex
  // buffer. Fragment is constant white. NDC: a centered upward triangle.
  const char* kShaderSource = R"(
    #include <metal_stdlib>
    using namespace metal;

    struct RasterData {
      float4 position [[position]];
    };

    vertex RasterData triangleVertex(uint vertexId [[vertex_id]]) {
      const float2 positions[3] = {
        float2( 0.0f,  0.8f),
        float2(-0.8f, -0.8f),
        float2( 0.8f, -0.8f),
      };
      RasterData out;
      out.position = float4(positions[vertexId], 0.0f, 1.0f);
      return out;
    }

    fragment float4 triangleFragment(RasterData in [[stage_in]]) {
      return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }
  )";

  // Builds the render pipeline state for the triangle. Returns a +1 reference
  // the caller owns, or nullptr on compile/link failure (reason printed).
  MTL::RenderPipelineState* buildPipeline(MTL::Device* device) {
    NS::Error* error = nullptr;

    NS::String* source = NS::String::string(kShaderSource, NS::UTF8StringEncoding);
    MTL::Library* library = device->newLibrary(source, nullptr, &error);
    if (!library) {
      printf("shader compile failed: %s\n",
             error ? error->localizedDescription()->utf8String() : "unknown");
      return nullptr;
    }

    MTL::Function* vertexFunction =
      library->newFunction(NS::String::string("triangleVertex", NS::UTF8StringEncoding));
    MTL::Function* fragmentFunction =
      library->newFunction(NS::String::string("triangleFragment", NS::UTF8StringEncoding));

    MTL::RenderPipelineDescriptor* descriptor =
      MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(vertexFunction);
    descriptor->setFragmentFunction(fragmentFunction);
    descriptor->colorAttachments()->object(0)->setPixelFormat(
      MTL::PixelFormatBGRA8Unorm);

    MTL::RenderPipelineState* pipeline =
      device->newRenderPipelineState(descriptor, &error);
    if (!pipeline) {
      printf("pipeline build failed: %s\n",
             error ? error->localizedDescription()->utf8String() : "unknown");
    }

    descriptor->release();
    fragmentFunction->release();
    vertexFunction->release();
    library->release();
    return pipeline;
  }

  // Allocates the offscreen color target the triangle renders into.
  MTL::Texture* createColorTarget(MTL::Device* device) {
    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setTextureType(MTL::TextureType2D);
    descriptor->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    descriptor->setWidth(kWidth);
    descriptor->setHeight(kHeight);
    descriptor->setStorageMode(MTL::StorageModeShared);  // unified memory readback
    descriptor->setUsage(MTL::TextureUsageRenderTarget);

    MTL::Texture* texture = device->newTexture(descriptor);
    descriptor->release();
    return texture;
  }

  // Writes a bottom-up 24-bit BMP from BGRA pixels. Preview-openable; keeps the
  // milestone dependency-free (no ImageIO/CoreGraphics).
  bool writeBmp(const char* path, const uint8_t* bgra, uint32_t width, uint32_t height) {
    const uint32_t rowStride = (width * 3 + 3) & ~3u;  // rows padded to 4 bytes
    const uint32_t pixelBytes = rowStride * height;
    const uint32_t fileBytes = 54 + pixelBytes;

    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    *reinterpret_cast<uint32_t*>(&header[2])  = fileBytes;
    *reinterpret_cast<uint32_t*>(&header[10]) = 54;          // pixel data offset
    *reinterpret_cast<uint32_t*>(&header[14]) = 40;          // DIB header size
    *reinterpret_cast<int32_t*>(&header[18])  = int32_t(width);
    *reinterpret_cast<int32_t*>(&header[22])  = int32_t(height);
    *reinterpret_cast<uint16_t*>(&header[26]) = 1;           // planes
    *reinterpret_cast<uint16_t*>(&header[28]) = 24;          // bits per pixel
    *reinterpret_cast<uint32_t*>(&header[34]) = pixelBytes;

    FILE* file = fopen(path, "wb");
    if (!file)
      return false;
    fwrite(header, 1, sizeof(header), file);

    std::vector<uint8_t> row(rowStride, 0);
    for (int32_t y = int32_t(height) - 1; y >= 0; --y) {  // BMP is bottom-up
      const uint8_t* src = bgra + size_t(y) * width * 4;
      for (uint32_t x = 0; x < width; ++x) {
        row[x * 3 + 0] = src[x * 4 + 0];  // B
        row[x * 3 + 1] = src[x * 4 + 1];  // G
        row[x * 3 + 2] = src[x * 4 + 2];  // R
      }
      fwrite(row.data(), 1, rowStride, file);
    }
    fclose(file);
    return true;
  }

}

int main() {
  NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

  d9mt::metal::MetalDevice gpu;
  if (!gpu.isValid()) {
    printf("no Metal device\n");
    return 1;
  }

  MTL::RenderPipelineState* pipeline = buildPipeline(gpu.device());
  MTL::Texture* colorTarget = createColorTarget(gpu.device());
  if (!pipeline || !colorTarget)
    return 1;

  MTL::RenderPassDescriptor* renderPass = MTL::RenderPassDescriptor::alloc()->init();
  MTL::RenderPassColorAttachmentDescriptor* colorAttachment =
    renderPass->colorAttachments()->object(0);
  colorAttachment->setTexture(colorTarget);
  colorAttachment->setLoadAction(MTL::LoadActionClear);
  colorAttachment->setStoreAction(MTL::StoreActionStore);
  colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));  // black

  MTL::CommandBuffer* commandBuffer = gpu.queue()->commandBuffer();
  MTL::RenderCommandEncoder* encoder =
    commandBuffer->renderCommandEncoder(renderPass);
  encoder->setRenderPipelineState(pipeline);
  encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                          NS::UInteger(0), NS::UInteger(3));
  encoder->endEncoding();
  commandBuffer->commit();
  commandBuffer->waitUntilCompleted();

  std::vector<uint8_t> pixels(size_t(kWidth) * kHeight * 4);
  colorTarget->getBytes(pixels.data(), kWidth * 4,
                        MTL::Region(0, 0, kWidth, kHeight), 0);

  const char* outputPath = "triangle.bmp";
  if (!writeBmp(outputPath, pixels.data(), kWidth, kHeight)) {
    printf("failed to write %s\n", outputPath);
    return 1;
  }
  printf("wrote %s (%ux%u)\n", outputPath, kWidth, kHeight);

  renderPass->release();
  colorTarget->release();
  pipeline->release();
  pool->release();
  return 0;
}
