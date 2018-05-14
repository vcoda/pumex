//
// Copyright(c) 2017-2018 Paweł Księżopolski ( pumexx )
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#include <pumex/CombinedImageSampler.h>
#include <pumex/Texture.h>
#include <pumex/Sampler.h>
#include <pumex/utils/Log.h>

using namespace pumex;

CombinedImageSampler::CombinedImageSampler(std::shared_ptr<ImageView> iv, std::shared_ptr<Sampler> s)
  : Resource{ iv->texture->getPerObjectBehaviour(), iv->texture->getSwapChainImageBehaviour() }, imageView{ iv }, sampler{ s }
{
  CHECK_LOG_THROW((iv->texture->getImageTraits().usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0, "Combined image sampler resource connected to a texture that does not have VK_IMAGE_USAGE_SAMPLED_BIT");
}

CombinedImageSampler::~CombinedImageSampler()
{
}

std::pair<bool, VkDescriptorType> CombinedImageSampler::getDefaultDescriptorType()
{
  return{ true, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
}

void CombinedImageSampler::validate(const RenderContext& renderContext)
{
  if (!registered)
  {
    imageView->addResource(shared_from_this());
    if(sampler!=nullptr)
      sampler->addResourceOwner(shared_from_this());
    registered = true;
  }
  imageView->validate(renderContext);
  if (sampler != nullptr)
    sampler->validate(renderContext);
}

DescriptorSetValue CombinedImageSampler::getDescriptorSetValue(const RenderContext& renderContext)
{
  return DescriptorSetValue(sampler->getHandleSampler(renderContext), imageView->getImageView(renderContext), VK_IMAGE_LAYOUT_UNDEFINED);
}
