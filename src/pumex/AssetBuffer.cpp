//
// Copyright(c) 2017-2018 Pawe� Ksi�opolski ( pumexx )
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

#include <pumex/AssetBuffer.h>
#include <set>
#include <pumex/Device.h>
#include <pumex/Node.h>
#include <pumex/PhysicalDevice.h>
#include <pumex/RenderContext.h>
#include <pumex/MemoryBuffer.h>
#include <pumex/Command.h>
#include <pumex/utils/Log.h>

namespace pumex
{

AssetBuffer::AssetBuffer(const std::vector<AssetBufferVertexSemantics>& vertexSemantics, std::shared_ptr<DeviceMemoryAllocator> bufferAllocator, std::shared_ptr<DeviceMemoryAllocator> vertexIndexAllocator)
{
  for (const auto& vs : vertexSemantics)
  {
    semantics[vs.renderMask]         = vs.vertexSemantic;
    perRenderMaskData[vs.renderMask] = PerRenderMaskData(bufferAllocator, vertexIndexAllocator);
  }

  typeNames.push_back("<null>");
  invTypeNames.insert({ "<null>", 0 });
  typeDefinitions.push_back(AssetTypeDefinition());
  lodDefinitions.push_back(std::vector<AssetLodDefinition>());
}

AssetBuffer::~AssetBuffer()
{
}

uint32_t AssetBuffer::registerType(const std::string& typeName, const AssetTypeDefinition& tdef)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (invTypeNames.find(typeName) != end(invTypeNames))
    return 0;
  uint32_t typeID = typeNames.size();
  typeNames.push_back(typeName);
  invTypeNames.insert({ typeName, typeID });
  typeDefinitions.push_back(tdef);
  lodDefinitions.push_back(std::vector<AssetLodDefinition>());
  invalidateNodeOwners();
  return typeID;
}

uint32_t AssetBuffer::registerObjectLOD(uint32_t typeID, std::shared_ptr<Asset> asset, const AssetLodDefinition& ldef)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (typeID == 0 || typeNames.size() < typeID)
    return UINT32_MAX;
  uint32_t lodID = lodDefinitions[typeID].size();
  lodDefinitions[typeID].push_back(ldef);

  // check if this asset has been registered already
  uint32_t assetIndex = assets.size();
  for (uint32_t i = 0; i < assets.size(); ++i)
  {
    if (asset.get() == assets[i].get())
    {
      assetIndex = i;
      break;
    }
  }
  if (assetIndex == assets.size())
    assets.push_back(asset);
  assetMapping.insert({ AssetKey(typeID,lodID), asset });

  for (uint32_t i = 0; i<assets[assetIndex]->geometries.size(); ++i)
    geometryDefinitions.push_back(InternalGeometryDefinition(typeID, lodID, assets[assetIndex]->geometries[i].renderMask, assetIndex, i));
  invalidateNodeOwners();
  return lodID;
}

uint32_t AssetBuffer::getTypeID(const std::string& typeName) const
{
  auto it = invTypeNames.find(typeName);
  if (it == end(invTypeNames))
    return 0;
  return it->second;
}

std::string AssetBuffer::getTypeName(uint32_t typeID) const
{
  if (typeID >= typeNames.size())
    return typeNames[0];
  return typeNames[typeID];
}

uint32_t AssetBuffer::getLodID(uint32_t typeID, float distance) const
{
  if (typeID == 0 || typeNames.size()<typeID)
    return UINT32_MAX;
  for (uint32_t i = 0; i < lodDefinitions[typeID].size(); ++i)
    if (lodDefinitions[typeID][i].active(distance))
      return i;
  return UINT32_MAX;
}

std::shared_ptr<Asset> AssetBuffer::getAsset(uint32_t typeID, uint32_t lodID)
{
  auto it = assetMapping.find(AssetKey(typeID, lodID));
  if (it != end(assetMapping))
    return it->second;
  return std::shared_ptr<Asset>();
}

void AssetBuffer::validate(const RenderContext& renderContext)
{
  std::lock_guard<std::mutex> lock(mutex);

  // divide geometries according to renderMasks
  std::map<uint32_t, std::vector<InternalGeometryDefinition>> geometryDefinitionsByRenderMask;
  for (const auto& gd : geometryDefinitions)
  {
    auto it = geometryDefinitionsByRenderMask.find(gd.renderMask);
    if (it == end(geometryDefinitionsByRenderMask))
      it = geometryDefinitionsByRenderMask.insert({ gd.renderMask, std::vector<InternalGeometryDefinition>() }).first;
    it->second.push_back(gd);
  }

  for (auto& gd : geometryDefinitionsByRenderMask)
  {
    // only create asset buffers for render masks that have nonempty vertex semantic defined
    auto pdmit = perRenderMaskData.find(gd.first);
    if (pdmit == end(perRenderMaskData))
      continue;
    PerRenderMaskData& rmData = pdmit->second;

    std::vector<VertexSemantic> requiredSemantic;
    auto sit = semantics.find(gd.first);
    if (sit != end(semantics))
      requiredSemantic = sit->second;
    if (requiredSemantic.empty())
      continue;

    // Sort geometries according to typeID and lodID
    std::sort(begin(gd.second), end(gd.second), [](const InternalGeometryDefinition& lhs, const InternalGeometryDefinition& rhs) { if (lhs.typeID != rhs.typeID) return lhs.typeID < rhs.typeID; return lhs.lodID < rhs.lodID; });

    VkDeviceSize                verticesSoFar   = 0;
    VkDeviceSize                indicesSoFar    = 0;
    rmData.vertices->resize(0);
    rmData.indices->resize(0);

    std::vector<AssetTypeDefinition>     assetTypes = typeDefinitions;
    std::vector<AssetLodDefinition>      assetLods;
    std::vector<AssetGeometryDefinition> assetGeometries;
    for (uint32_t t = 0; t < assetTypes.size(); ++t)
    {
      auto typePair = std::equal_range(begin(gd.second), end(gd.second), InternalGeometryDefinition(t, 0, 0, 0, 0), [](const InternalGeometryDefinition& lhs, const InternalGeometryDefinition& rhs) {return lhs.typeID < rhs.typeID; });
      assetTypes[t].lodFirst = assetLods.size();
      for (uint32_t l = 0; l < lodDefinitions[t].size(); ++l)
      {
        auto lodPair = std::equal_range(typePair.first, typePair.second, InternalGeometryDefinition(t, l, 0, 0, 0), [](const InternalGeometryDefinition& lhs, const InternalGeometryDefinition& rhs) {return lhs.lodID < rhs.lodID; });
        if (lodPair.first != lodPair.second)
        {
          AssetLodDefinition lodDef = lodDefinitions[t][l];
          lodDef.geomFirst = assetGeometries.size();
          for (auto it = lodPair.first; it != lodPair.second; ++it)
          {
            uint32_t indexCount = assets[it->assetIndex]->geometries[it->geometryIndex].getIndexCount();
            uint32_t firstIndex = indicesSoFar;
            uint32_t vertexOffset = verticesSoFar;
            assetGeometries.push_back(AssetGeometryDefinition(indexCount, firstIndex, vertexOffset));

            // calculating buffer sizes etc
            verticesSoFar += assets[it->assetIndex]->geometries[it->geometryIndex].getVertexCount();
            indicesSoFar += indexCount;

            // copying vertices to a vertex buffer
            copyAndConvertVertices(*(rmData.vertices), requiredSemantic, assets[it->assetIndex]->geometries[it->geometryIndex].vertices, assets[it->assetIndex]->geometries[it->geometryIndex].semantic);
            // copying indices to an index buffer
            const auto& indices = assets[it->assetIndex]->geometries[it->geometryIndex].indices;
            std::copy(begin(indices), end(indices), std::back_inserter(*(rmData.indices)));
          }
          lodDef.geomSize = assetGeometries.size() - lodDef.geomFirst;
          assetLods.push_back(lodDef);
        }
      }
      assetTypes[t].lodSize = assetLods.size() - assetTypes[t].lodFirst;
    }
    rmData.vertexBuffer->invalidateData();
    rmData.indexBuffer->invalidateData();
    (*rmData.aTypes)    = assetTypes;
    (*rmData.aLods)     = assetLods;
    (*rmData.aGeomDefs) = assetGeometries;
    rmData.typeBuffer->invalidateData();
    rmData.lodBuffer->invalidateData();
    rmData.geomBuffer->invalidateData();
  }
  for (auto& prm : perRenderMaskData)
  {
    prm.second.vertexBuffer->validate(renderContext);
    prm.second.indexBuffer->validate(renderContext);
  }
}

void AssetBuffer::cmdBindVertexIndexBuffer(const RenderContext& renderContext, CommandBuffer* commandBuffer, uint32_t renderMask, uint32_t vertexBinding)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto prmit = perRenderMaskData.find(renderMask);
  if (prmit == end(perRenderMaskData))
  {
    LOG_WARNING << "AssetBuffer::bindVertexIndexBuffer() does not have this render mask defined" << std::endl;
    return;
  }
  VkBuffer vBuffer = prmit->second.vertexBuffer->getHandleBuffer(renderContext);
  VkBuffer iBuffer = prmit->second.indexBuffer->getHandleBuffer(renderContext);
  VkDeviceSize offsets = 0;
  vkCmdBindVertexBuffers(commandBuffer->getHandle(), vertexBinding, 1, &vBuffer, &offsets);
  vkCmdBindIndexBuffer(commandBuffer->getHandle(), iBuffer, 0, VK_INDEX_TYPE_UINT32);
}

void AssetBuffer::cmdDrawObject(const RenderContext& renderContext, CommandBuffer* commandBuffer, uint32_t renderMask, uint32_t typeID, uint32_t firstInstance, float distanceToViewer) const
{
  std::lock_guard<std::mutex> lock(mutex);

  auto prmit = perRenderMaskData.find(renderMask);
  if (prmit == end(perRenderMaskData))
  {
    LOG_WARNING << "AssetBuffer::drawObject() does not have this render mask defined" << std::endl;
    return;
  }
  auto& assetTypes      = *prmit->second.aTypes;
  auto& assetLods       = *prmit->second.aLods;
  auto& assetGeometries = *prmit->second.aGeomDefs;

  uint32_t lodFirst = assetTypes[typeID].lodFirst;
  uint32_t lodSize  = assetTypes[typeID].lodSize;
  for (unsigned int l = lodFirst; l < lodFirst + lodSize; ++l)
  {
    if (assetLods[l].active(distanceToViewer))
    {
      uint32_t geomFirst = assetLods[l].geomFirst;
      uint32_t geomSize  = assetLods[l].geomSize;
      for (uint32_t g = geomFirst; g < geomFirst + geomSize; ++g)
      {
        uint32_t indexCount   = assetGeometries[g].indexCount;
        uint32_t firstIndex   = assetGeometries[g].firstIndex;
        uint32_t vertexOffset = assetGeometries[g].vertexOffset;
        commandBuffer->cmdDrawIndexed(indexCount, 1, firstIndex, vertexOffset, firstInstance);
      }
    }
  }
}

void AssetBuffer::cmdDrawObjectsIndirect(const RenderContext& renderContext, CommandBuffer* commandBuffer, uint32_t renderMask, std::shared_ptr<AssetBufferInstancedResults> instancedResults)
{
  std::lock_guard<std::mutex> lock(mutex);

  auto buffer = instancedResults->getResults(renderMask)->getHandleBuffer(renderContext);

  uint32_t drawCount = instancedResults->getDrawCount(renderMask);

  if (renderContext.device->physical.lock()->features.multiDrawIndirect == 1)
    commandBuffer->cmdDrawIndexedIndirect(buffer, 0, drawCount, sizeof(DrawIndexedIndirectCommand));
  else
  {
    for (uint32_t i = 0; i < drawCount; ++i)
      commandBuffer->cmdDrawIndexedIndirect(buffer, 0 + i * sizeof(DrawIndexedIndirectCommand), 1, sizeof(DrawIndexedIndirectCommand));
  }
}

std::shared_ptr<Buffer<std::vector<AssetTypeDefinition>>> AssetBuffer::getTypeBuffer(uint32_t renderMask)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto it = perRenderMaskData.find(renderMask);
  CHECK_LOG_THROW(it == end(perRenderMaskData), "AssetBuffer::getTypeBuffer() attempting to get a buffer for nonexisting render mask");
  return it->second.typeBuffer;
}
std::shared_ptr<Buffer<std::vector<AssetLodDefinition>>> AssetBuffer::getLodBuffer(uint32_t renderMask)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto it = perRenderMaskData.find(renderMask);
  CHECK_LOG_THROW(it == end(perRenderMaskData), "AssetBuffer::getLodBuffer() attempting to get a buffer for nonexisting render mask");
  return it->second.lodBuffer;
}
std::shared_ptr<Buffer<std::vector<AssetGeometryDefinition>>> AssetBuffer::getGeomBuffer(uint32_t renderMask)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto it = perRenderMaskData.find(renderMask);
  CHECK_LOG_THROW(it == end(perRenderMaskData), "AssetBuffer::getGeomBuffer() attempting to get a buffer for nonexisting render mask");
  return it->second.geomBuffer;
}

void AssetBuffer::prepareDrawIndexedIndirectCommandBuffer(uint32_t renderMask, std::vector<DrawIndexedIndirectCommand>& resultBuffer, std::vector<uint32_t>& resultGeomToType) const
{
  resultBuffer.resize(0);
  resultGeomToType.resize(0);
  std::vector<InternalGeometryDefinition> geomDefinitions;
  for (const auto& gd : geometryDefinitions)
  {
    if (gd.renderMask == renderMask)
      geomDefinitions.push_back(gd);
  }

  std::sort(begin(geomDefinitions), end(geomDefinitions), [](const InternalGeometryDefinition& lhs, const InternalGeometryDefinition& rhs){ if (lhs.typeID != rhs.typeID) return lhs.typeID < rhs.typeID; return lhs.lodID < rhs.lodID; });

  VkDeviceSize                       verticesSoFar = 0;
  VkDeviceSize                       indicesSoFar = 0;
  VkDeviceSize                       indexBufferSize = 0;

  std::vector<AssetLodDefinition>      assetLods;
  std::vector<AssetGeometryDefinition> assetGeometries;
  for (uint32_t t = 0; t < typeDefinitions.size(); ++t)
  {
    auto typePair = std::equal_range(begin(geomDefinitions), end(geomDefinitions), InternalGeometryDefinition(t, 0, 0, 0, 0), [](const InternalGeometryDefinition& lhs, const InternalGeometryDefinition& rhs){return lhs.typeID < rhs.typeID; });
    for (uint32_t l = 0; l<lodDefinitions[t].size(); ++l)
    {
      auto lodPair = std::equal_range(typePair.first, typePair.second, InternalGeometryDefinition(t, l, 0, 0, 0), [](const InternalGeometryDefinition& lhs, const InternalGeometryDefinition& rhs){return lhs.lodID < rhs.lodID; });
      if (lodPair.first != lodPair.second)
      {
        for (auto it = lodPair.first; it != lodPair.second; ++it)
        {
          uint32_t indexCount   = assets[it->assetIndex]->geometries[it->geometryIndex].getIndexCount();
          uint32_t firstIndex   = indicesSoFar;
          uint32_t vertexOffset = verticesSoFar;
          resultBuffer.push_back(DrawIndexedIndirectCommand(indexCount, 0, firstIndex, vertexOffset, 0));
          resultGeomToType.push_back(t);

          verticesSoFar += assets[it->assetIndex]->geometries[it->geometryIndex].getVertexCount();
          indicesSoFar  += indexCount;

        }
      }
    }
  }
}

void AssetBuffer::addNodeOwner(std::shared_ptr<Node> node)
{
  if (std::find_if(begin(nodeOwners), end(nodeOwners), [&node](std::weak_ptr<Node> n) { return !n.expired() && n.lock().get() == node.get(); }) == end(nodeOwners))
    nodeOwners.push_back(node);
}

void AssetBuffer::invalidateNodeOwners()
{
  auto eit = std::remove_if(begin(nodeOwners), end(nodeOwners), [](std::weak_ptr<Node> n) { return n.expired();  });
  for (auto it = begin(nodeOwners); it != eit; ++it)
    it->lock()->invalidateNodeAndParents();
  nodeOwners.erase(eit, end(nodeOwners));
}


AssetBuffer::PerRenderMaskData::PerRenderMaskData(std::shared_ptr<DeviceMemoryAllocator> bufferAllocator, std::shared_ptr<DeviceMemoryAllocator> vertexIndexAllocator)
{
  vertices     = std::make_shared<std::vector<float>>();
  indices      = std::make_shared<std::vector<uint32_t>>();
  vertexBuffer = std::make_shared<Buffer<std::vector<float>>>(vertices, vertexIndexAllocator, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, pbPerDevice, swForEachImage);
  indexBuffer  = std::make_shared<Buffer<std::vector<uint32_t>>>(indices, vertexIndexAllocator, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, pbPerDevice, swForEachImage);

  aTypes       = std::make_shared<std::vector<AssetTypeDefinition>>();
  aLods        = std::make_shared<std::vector<AssetLodDefinition>>();
  aGeomDefs    = std::make_shared<std::vector<AssetGeometryDefinition>>();
  typeBuffer   = std::make_shared<Buffer<std::vector<AssetTypeDefinition>>>(aTypes, bufferAllocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, pbPerDevice, swForEachImage);
  lodBuffer    = std::make_shared<Buffer<std::vector<AssetLodDefinition>>>(aLods, bufferAllocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, pbPerDevice, swForEachImage);
  geomBuffer   = std::make_shared<Buffer<std::vector<AssetGeometryDefinition>>>(aGeomDefs, bufferAllocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, pbPerDevice, swForEachImage);
}

AssetBufferInstancedResults::AssetBufferInstancedResults(const std::vector<AssetBufferVertexSemantics>& vertexSemantics, std::shared_ptr<AssetBuffer> ab, std::shared_ptr<DeviceMemoryAllocator> buffersAllocator)
  : assetBuffer{ ab }
{
  for (const auto& vs : vertexSemantics)
  {
    semantics[vs.renderMask]         = vs.vertexSemantic;
    perRenderMaskData[vs.renderMask] = PerRenderMaskData(buffersAllocator);
  }
}

AssetBufferInstancedResults::~AssetBufferInstancedResults()
{
}

void AssetBufferInstancedResults::setup()
{
  auto ab = assetBuffer.lock();
  for (auto& prm : perRenderMaskData)
  {
    ab->prepareDrawIndexedIndirectCommandBuffer(prm.first, prm.second.initialResultValues, prm.second.resultsGeomToType);
  }
}

void AssetBufferInstancedResults::prepareBuffers(const std::vector<uint32_t>& typeCount)
{
  for (auto& prm : perRenderMaskData)
  {
    PerRenderMaskData& rmData = prm.second;
    std::vector<uint32_t> offsets;
    for (uint32_t i = 0; i < rmData.resultsGeomToType.size(); ++i)
      offsets.push_back(typeCount[rmData.resultsGeomToType[i]]);

    std::vector<DrawIndexedIndirectCommand> results = rmData.initialResultValues;
    uint32_t offsetSum = 0;
    for (uint32_t i = 0; i < offsets.size(); ++i)
    {
      uint32_t tmp = offsetSum;
      offsetSum += offsets[i];
      offsets[i] = tmp;
      results[i].firstInstance = tmp;
    }
    rmData.resultsBuffer->setData(results);
    rmData.offValuesBuffer->setData(std::vector<uint32_t>(offsetSum));
  }
}

std::shared_ptr<Buffer<std::vector<DrawIndexedIndirectCommand>>> AssetBufferInstancedResults::getResults(uint32_t renderMask)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto it = perRenderMaskData.find(renderMask);
  CHECK_LOG_THROW(it == end(perRenderMaskData), "AssetBufferInstancedResults::getResults() attempting to get a buffer for nonexisting render mask");
  return it->second.resultsBuffer;
}

std::shared_ptr<Buffer<std::vector<uint32_t>>> AssetBufferInstancedResults::getOffsetValues(uint32_t renderMask)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto it = perRenderMaskData.find(renderMask);
  CHECK_LOG_THROW(it == end(perRenderMaskData), "AssetBufferInstancedResults::getOffsetValues() attempting to get a buffer for nonexisting render mask");
  return it->second.offValuesBuffer;
}

uint32_t AssetBufferInstancedResults::getDrawCount(uint32_t renderMask)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto it = perRenderMaskData.find(renderMask);
  CHECK_LOG_THROW(it == end(perRenderMaskData), "AssetBufferInstancedResults::getDrawCount() attempting to get a draw count for nonexisting render mask");
  return it->second.initialResultValues.size();
}

void AssetBufferInstancedResults::validate(const RenderContext& renderContext)
{
  for (auto& prm : perRenderMaskData)
  {
    PerRenderMaskData& rmData = prm.second;
    rmData.resultsBuffer->validate(renderContext);
    rmData.offValuesBuffer->validate(renderContext);
  }
}

AssetBufferInstancedResults::PerRenderMaskData::PerRenderMaskData(std::shared_ptr<DeviceMemoryAllocator> allocator)
{
  resultsBuffer   = std::make_shared<Buffer<std::vector<DrawIndexedIndirectCommand>>>(std::make_shared<std::vector<DrawIndexedIndirectCommand>>(), allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, pbPerSurface, swForEachImage);
  offValuesBuffer = std::make_shared<Buffer<std::vector<uint32_t>>>(std::make_shared<std::vector<uint32_t>>(), allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, pbPerSurface, swForEachImage);
}

}