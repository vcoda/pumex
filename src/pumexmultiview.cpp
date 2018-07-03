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

#include <iomanip>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <pumex/Pumex.h>
#include <pumex/AssetLoaderAssimp.h>
#include <pumex/utils/Shapes.h>
#include <args.hxx>

// This example uses code from deferred example ( renders the same scene using deferred rendering )
// The real purpose of this example is how to render to multiview device ( Oculus VR for example ).
//
// Main differences between normal rendering and multiview rendering in Vulkan:
// - vulkan instance ( Viewer class ) has to use VK_KHR_get_physical_device_properties2 extension
// - logical device ( Device class ) has to use VK_KHR_multiview extension
// - render operations that use multiview extension have to have multiview mask set to values other than 0U
// - shaders used in these render operations must enable GL_EXT_multiview extension to use gl_ViewIndex variable
//
// Keep in mind that this example is not full Oculus ready, because :
// - Oculus SDK uses its own functions replacing some of the functions from Vulkan SDK ( swapchain image acquirement, swapchain image presentation, etc )
// - Oculus performs barrel distortion itself, so you don't have to really do this. Example performs barrel distortion for educational purposes

const uint32_t              MAX_BONES = 511;
const VkSampleCountFlagBits SAMPLE_COUNT = VK_SAMPLE_COUNT_2_BIT;
const uint32_t              MODEL_SPONZA_ID = 1;

struct PositionData
{
  PositionData()
  {
  }
  PositionData(const glm::mat4& p)
    : position{ p }
  {
  }
  glm::mat4 position;
  glm::mat4 bones[MAX_BONES];
  uint32_t  typeID;
  // std430 ?
};

struct MaterialData
{
  uint32_t  diffuseTextureIndex   = 0;
  uint32_t  roughnessTextureIndex = 0;
  uint32_t  metallicTextureIndex  = 0;
  uint32_t  normalTextureIndex    = 0;

  // two functions that define material parameters according to data from an asset's material 
  void registerProperties(const pumex::Material& material)
  {
  }
  void registerTextures(const std::map<pumex::TextureSemantic::Type, uint32_t>& textureIndices)
  {
    auto it = textureIndices.find(pumex::TextureSemantic::Diffuse);
    diffuseTextureIndex   = (it == end(textureIndices)) ? 0 : it->second;
    it = textureIndices.find(pumex::TextureSemantic::Specular);
    roughnessTextureIndex = (it == end(textureIndices)) ? 0 : it->second;
    it = textureIndices.find(pumex::TextureSemantic::LightMap);
    metallicTextureIndex  = (it == end(textureIndices)) ? 0 : it->second;
    it = textureIndices.find(pumex::TextureSemantic::Normals);
    normalTextureIndex    = (it == end(textureIndices)) ? 0 : it->second;
  }
};

// simple light point sent to GPU in a storage buffer
struct LightPointData
{
  LightPointData()
  {
  }

  LightPointData(const glm::vec3& pos, const glm::vec3& col, const glm::vec3& att)
    : position{ pos.x, pos.y, pos.z, 0.0f }, color{ col.x, col.y, col.z, 1.0f }, attenuation{ att.x, att.y, att.z, 1.0f }
  {
  }
  glm::vec4 position;
  glm::vec4 color;
  glm::vec4 attenuation;
};

struct UpdateData
{
  UpdateData()
  {
  }
  glm::vec3 cameraPosition;
  glm::vec2 cameraGeographicCoordinates;
  float     cameraDistance;

  glm::vec2 lastMousePos;
  bool      leftMouseKeyPressed;
  bool      rightMouseKeyPressed;
  
  bool      moveForward;
  bool      moveBackward;
  bool      moveLeft;
  bool      moveRight;
  bool      moveUp;
  bool      moveDown;
  bool      moveFast;

};

struct RenderData
{
  RenderData()
    : prevCameraDistance{ 1.0f }, cameraDistance{ 1.0f }
  {
  }
  glm::vec3               prevCameraPosition;
  glm::vec2               prevCameraGeographicCoordinates;
  float                   prevCameraDistance;
  glm::vec3               cameraPosition;
  glm::vec2               cameraGeographicCoordinates;
  float                   cameraDistance;

};

struct DeferredApplicationData
{
  DeferredApplicationData(std::shared_ptr<pumex::DeviceMemoryAllocator> buffersAllocator)
  {
    cameraBuffer     = std::make_shared<pumex::Buffer<std::vector<pumex::Camera>>>(buffersAllocator, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, pumex::pbPerSurface, pumex::swOnce, true);
    textCameraBuffer = std::make_shared<pumex::Buffer<std::vector<pumex::Camera>>>(buffersAllocator, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, pumex::pbPerSurface, pumex::swOnce, true);
    positionData     = std::make_shared<PositionData>();
    positionBuffer   = std::make_shared<pumex::Buffer<PositionData>>(positionData, buffersAllocator, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, pumex::pbPerDevice, pumex::swOnce);

    auto lights = std::make_shared<std::vector<LightPointData>>();
    lights->push_back( LightPointData(glm::vec3(-6.178, -1.434, 1.439), glm::vec3(5.0, 5.0, 5.0), glm::vec3(0.0, 0.0, 1.0)) );
    lights->push_back( LightPointData(glm::vec3(-6.178, 2.202, 1.439),  glm::vec3(5.0, 0.1, 0.1), glm::vec3(0.0, 0.0, 1.0)) );
    lights->push_back( LightPointData(glm::vec3(4.883, 2.202, 1.439),   glm::vec3(0.1, 0.1, 5.0), glm::vec3(0.0, 0.0, 1.0)) );
    lights->push_back( LightPointData(glm::vec3(4.883, -1.434, 1.439),  glm::vec3(0.1, 5.0, 0.1), glm::vec3(0.0, 0.0, 1.0)) );
    lightsBuffer = std::make_shared<pumex::Buffer<std::vector<LightPointData>>>(lights, buffersAllocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, pumex::pbPerDevice, pumex::swOnce);
    lastFrameStart = pumex::HPClock::now();

    updateData.cameraPosition              = glm::vec3(0.0f, 0.0f, 0.5f);
    updateData.cameraGeographicCoordinates = glm::vec2(0.0f, 0.0f);
    updateData.cameraDistance              = 0.6f;
    updateData.leftMouseKeyPressed         = false;
    updateData.rightMouseKeyPressed        = false;
    updateData.moveForward                 = false;
    updateData.moveBackward                = false;
    updateData.moveLeft                    = false;
    updateData.moveRight                   = false;
    updateData.moveUp                      = false;
    updateData.moveDown                    = false;
    updateData.moveFast                    = false;
  }

  void processInput(std::shared_ptr<pumex::Surface> surface)
  {
    std::shared_ptr<pumex::Window> window = surface->window.lock();
    std::shared_ptr<pumex::Viewer> viewer = surface->viewer.lock();

    std::vector<pumex::InputEvent> mouseEvents = window->getInputEvents();
    glm::vec2 mouseMove = updateData.lastMousePos;
    for (const auto& m : mouseEvents)
    {
      switch (m.type)
      {
      case pumex::InputEvent::MOUSE_KEY_PRESSED:
        if (m.mouseButton == pumex::InputEvent::LEFT)
          updateData.leftMouseKeyPressed = true;
        if (m.mouseButton == pumex::InputEvent::RIGHT)
          updateData.rightMouseKeyPressed = true;
        mouseMove.x = m.x;
        mouseMove.y = m.y;
        updateData.lastMousePos = mouseMove;
        break;
      case pumex::InputEvent::MOUSE_KEY_RELEASED:
        if (m.mouseButton == pumex::InputEvent::LEFT)
          updateData.leftMouseKeyPressed = false;
        if (m.mouseButton == pumex::InputEvent::RIGHT)
          updateData.rightMouseKeyPressed = false;
        break;
      case pumex::InputEvent::MOUSE_MOVE:
        if (updateData.leftMouseKeyPressed || updateData.rightMouseKeyPressed)
        {
          mouseMove.x = m.x;
          mouseMove.y = m.y;
        }
        break;
      case pumex::InputEvent::KEYBOARD_KEY_PRESSED:
        switch(m.key)
        {
        case pumex::InputEvent::W:     updateData.moveForward  = true; break;
        case pumex::InputEvent::S:     updateData.moveBackward = true; break;
        case pumex::InputEvent::A:     updateData.moveLeft     = true; break;
        case pumex::InputEvent::D:     updateData.moveRight    = true; break;
        case pumex::InputEvent::Q:     updateData.moveUp       = true; break;
        case pumex::InputEvent::Z:     updateData.moveDown     = true; break;
        case pumex::InputEvent::SHIFT: updateData.moveFast     = true; break;
        }
        break;
      case pumex::InputEvent::KEYBOARD_KEY_RELEASED:
        switch(m.key)
        {
        case pumex::InputEvent::W:     updateData.moveForward  = false; break;
        case pumex::InputEvent::S:     updateData.moveBackward = false; break;
        case pumex::InputEvent::A:     updateData.moveLeft     = false; break;
        case pumex::InputEvent::D:     updateData.moveRight    = false; break;
        case pumex::InputEvent::Q:     updateData.moveUp       = false; break;
        case pumex::InputEvent::Z:     updateData.moveDown     = false; break;
        case pumex::InputEvent::SHIFT: updateData.moveFast     = false; break;
        }
        break;
      }
    }

    uint32_t updateIndex = viewer->getUpdateIndex();
    RenderData& uData = renderData[updateIndex];
    uData.prevCameraGeographicCoordinates = updateData.cameraGeographicCoordinates;
    uData.prevCameraDistance = updateData.cameraDistance;
    uData.prevCameraPosition = updateData.cameraPosition;

    if (updateData.leftMouseKeyPressed)
    {
      updateData.cameraGeographicCoordinates.x -= 100.0f*(mouseMove.x - updateData.lastMousePos.x);
      updateData.cameraGeographicCoordinates.y += 100.0f*(mouseMove.y - updateData.lastMousePos.y);
      while (updateData.cameraGeographicCoordinates.x < -180.0f)
        updateData.cameraGeographicCoordinates.x += 360.0f;
      while (updateData.cameraGeographicCoordinates.x>180.0f)
        updateData.cameraGeographicCoordinates.x -= 360.0f;
      updateData.cameraGeographicCoordinates.y = glm::clamp(updateData.cameraGeographicCoordinates.y, -90.0f, 90.0f);
      updateData.lastMousePos = mouseMove;
    }
    if (updateData.rightMouseKeyPressed)
    {
      updateData.cameraDistance += 10.0f*(updateData.lastMousePos.y - mouseMove.y);
      if (updateData.cameraDistance<0.1f)
        updateData.cameraDistance = 0.1f;
      updateData.lastMousePos = mouseMove;
    }

    float camSpeed = 0.2f;
    if (updateData.moveFast)
      camSpeed = 1.0f;
    glm::vec3 forward = glm::vec3(cos(updateData.cameraGeographicCoordinates.x * 3.1415f / 180.0f), sin(updateData.cameraGeographicCoordinates.x * 3.1415f / 180.0f), 0);
    glm::vec3 right   = glm::vec3(cos((updateData.cameraGeographicCoordinates.x + 90.0f) * 3.1415f / 180.0f), sin((updateData.cameraGeographicCoordinates.x + 90.0f) * 3.1415f / 180.0f), 0);
    glm::vec3 up      = glm::vec3(0.0f, 0.0f, 1.0f);
    if (updateData.moveForward)
      updateData.cameraPosition -= forward * camSpeed;
    if (updateData.moveBackward)
      updateData.cameraPosition += forward * camSpeed;
    if (updateData.moveLeft)
      updateData.cameraPosition -= right * camSpeed;
    if (updateData.moveRight)
      updateData.cameraPosition += right * camSpeed;
    if (updateData.moveUp)
      updateData.cameraPosition += up * camSpeed;
    if (updateData.moveDown)
      updateData.cameraPosition -= up * camSpeed;

    uData.cameraGeographicCoordinates = updateData.cameraGeographicCoordinates;
    uData.cameraDistance = updateData.cameraDistance;
    uData.cameraPosition = updateData.cameraPosition;
  }

  void update(double timeSinceStart, double updateStep)
  {
  }

  void prepareCameraForRendering(std::shared_ptr<pumex::Surface> surface)
  {
    std::shared_ptr<pumex::Viewer> viewer = surface->viewer.lock();
    uint32_t renderIndex = viewer->getRenderIndex();
    const RenderData& rData = renderData[renderIndex];

    float deltaTime = pumex::inSeconds(viewer->getRenderTimeDelta());
    float renderTime = pumex::inSeconds(viewer->getUpdateTime() - viewer->getApplicationStartTime()) + deltaTime;

    glm::vec3 relCam
    (
      rData.cameraDistance * cos(rData.cameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.cameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.cameraDistance * sin(rData.cameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.cameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.cameraDistance * sin(rData.cameraGeographicCoordinates.y * 3.1415f / 180.0f)
    );
    glm::vec3 prevRelCam
    (
      rData.prevCameraDistance * cos(rData.prevCameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.prevCameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.prevCameraDistance * sin(rData.prevCameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.prevCameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.prevCameraDistance * sin(rData.prevCameraGeographicCoordinates.y * 3.1415f / 180.0f)
    );
    glm::vec3 eye = relCam + rData.cameraPosition;
    glm::vec3 prevEye = prevRelCam + rData.prevCameraPosition;

    glm::vec3 realEye = eye + deltaTime * (eye - prevEye);
    glm::vec3 realCenter = rData.cameraPosition + deltaTime * (rData.cameraPosition - rData.prevCameraPosition);

    glm::mat4 viewMatrix = glm::lookAt(realEye, realCenter, glm::vec3(0, 0, 1));

    uint32_t renderWidth = surface->swapChainSize.width;
    uint32_t renderHeight = surface->swapChainSize.height;
    std::vector<pumex::Camera> cameras;
    {
      pumex::Camera camera;
      camera.setViewMatrix(glm::translate(glm::mat4(), glm::vec3(0.0325f, 0.0f, 0.0f)) * viewMatrix);
      camera.setObserverPosition(realEye);
      camera.setTimeSinceStart(renderTime);
      camera.setProjectionMatrix(glm::perspective(glm::radians(60.0f), 0.5f * (float)renderWidth / (float)renderHeight, 0.1f, 10000.0f));
      cameras.push_back(camera);
      camera.setViewMatrix(glm::translate(glm::mat4(), glm::vec3(-0.0325f, 0.0f, 0.0f)) * viewMatrix);
      cameras.push_back(camera);
    }
    cameraBuffer->setData(surface.get(), cameras);

    std::vector<pumex::Camera> textCameras;
    {
      pumex::Camera textCamera;
      textCamera.setProjectionMatrix(glm::ortho(0.0f, (float)renderWidth, 0.0f, (float)renderHeight), false);
      textCameras.push_back(textCamera);
      textCameras.push_back(textCamera);
    }
    textCameraBuffer->setData(surface.get(), textCameras);
  }

  void prepareModelForRendering(std::shared_ptr<pumex::Viewer> viewer, std::shared_ptr<pumex::AssetBuffer> assetBuffer, uint32_t modelTypeID)
  {
    fillFPS(viewer);

    std::shared_ptr<pumex::Asset> assetX = assetBuffer->getAsset(modelTypeID, 0);
    if (assetX->animations.empty())
      return;

    uint32_t renderIndex = viewer->getRenderIndex();
    const RenderData& rData = renderData[renderIndex];

    float deltaTime = pumex::inSeconds(viewer->getRenderTimeDelta());
    float renderTime = pumex::inSeconds(viewer->getUpdateTime() - viewer->getApplicationStartTime()) + deltaTime;

    pumex::Animation& anim = assetX->animations[0];
    pumex::Skeleton& skel = assetX->skeleton;

    uint32_t numAnimChannels = anim.channels.size();
    uint32_t numSkelBones = skel.bones.size();

    std::vector<uint32_t> boneChannelMapping(numSkelBones);
    for (uint32_t boneIndex = 0; boneIndex < numSkelBones; ++boneIndex)
    {
      auto it = anim.invChannelNames.find(skel.boneNames[boneIndex]);
      boneChannelMapping[boneIndex] = (it != end(anim.invChannelNames)) ? it->second : UINT32_MAX;
    }

    std::vector<glm::mat4> localTransforms(MAX_BONES);
    std::vector<glm::mat4> globalTransforms(MAX_BONES);

    anim.calculateLocalTransforms(renderTime, localTransforms.data(), numAnimChannels);
    uint32_t bcVal = boneChannelMapping[0];
    glm::mat4 localCurrentTransform = (bcVal == UINT32_MAX) ? skel.bones[0].localTransformation : localTransforms[bcVal];
    globalTransforms[0] = skel.invGlobalTransform * localCurrentTransform;
    for (uint32_t boneIndex = 1; boneIndex < numSkelBones; ++boneIndex)
    {
      bcVal = boneChannelMapping[boneIndex];
      localCurrentTransform = (bcVal == UINT32_MAX) ? skel.bones[boneIndex].localTransformation : localTransforms[bcVal];
      globalTransforms[boneIndex] = globalTransforms[skel.bones[boneIndex].parentIndex] * localCurrentTransform;
    }
    for (uint32_t boneIndex = 0; boneIndex < numSkelBones; ++boneIndex)
      positionData->bones[boneIndex] = globalTransforms[boneIndex] * skel.bones[boneIndex].offsetMatrix;

    positionBuffer->invalidateData();
  }

  void finishFrame(std::shared_ptr<pumex::Viewer> viewer, std::shared_ptr<pumex::Surface> surface)
  {
  }

  void fillFPS(std::shared_ptr<pumex::Viewer> viewer)
  {
    pumex::HPClock::time_point thisFrameStart = pumex::HPClock::now();
    double fpsValue = 1.0 / pumex::inSeconds(thisFrameStart - lastFrameStart);
    lastFrameStart = thisFrameStart;

    //std::wstringstream stream;
    //stream << "FPS : " << std::fixed << std::setprecision(1) << fpsValue;
    //textDefault->setText(viewer->getSurface(0), 0, glm::vec2(30, 28), glm::vec4(1.0f, 1.0f, 0.0f, 1.0f), stream.str());
  }

  UpdateData                                            updateData;
  std::array<RenderData, 3>                             renderData;

  std::shared_ptr<pumex::Buffer<std::vector<pumex::Camera>>> cameraBuffer;
  std::shared_ptr<pumex::Buffer<std::vector<pumex::Camera>>> textCameraBuffer;
  std::shared_ptr<PositionData>                              positionData;
  std::shared_ptr<pumex::Buffer<PositionData>>               positionBuffer;

  std::shared_ptr<pumex::Buffer<std::vector<LightPointData>>> lightsBuffer;
  pumex::HPClock::time_point                            lastFrameStart;
  std::shared_ptr<pumex::Text>                          textDefault;
};

std::shared_ptr<pumex::Asset> buildMultiViewQuads()
{
  auto result = std::make_shared<pumex::Asset>();
  std::vector<pumex::VertexSemantic> vertexSemantic = { { pumex::VertexSemantic::Position, 3 },{ pumex::VertexSemantic::TexCoord, 3 }};

  pumex::Geometry quads;
  quads.name = "multiview_quads";
  quads.semantic = vertexSemantic;
  pumex::addQuad(quads, glm::vec3(-1.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -2.0f, 0.0f), 0.0f, 1.0f, 1.0f, 0.0f, 0.0f);
  pumex::addQuad(quads, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -2.0f, 0.0f), 0.0f, 1.0f, 1.0f, 0.0f, 1.0f);
  result->geometries.push_back(quads);

  pumex::Skeleton::Bone bone;
  result->skeleton.bones.emplace_back(bone);
  result->skeleton.boneNames.push_back("root");
  result->skeleton.invBoneNames.insert({ "root", 0 });

  return result;
}

int main( int argc, char * argv[] )
{
  SET_LOG_INFO;

  args::ArgumentParser parser("pumex example : multiview deferred rendering with PBR and antialiasing");
  args::HelpFlag       help(parser, "help", "display this help menu", { 'h', "help" });
  args::Flag           enableDebugging(parser, "debug", "enable Vulkan debugging", { 'd' });
  args::Flag           useFullScreen(parser, "fullscreen", "create fullscreen window", { 'f' });
  try
  {
    parser.ParseCLI(argc, argv);
  }
  catch (const args::Help&)
  {
    LOG_ERROR << parser;
    FLUSH_LOG;
    return 0;
  }
  catch (const args::ParseError& e)
  {
    LOG_ERROR << e.what() << std::endl;
    LOG_ERROR << parser;
    FLUSH_LOG;
    return 1;
  }
  catch (const args::ValidationError& e)
  {
    LOG_ERROR << e.what() << std::endl;
    LOG_ERROR << parser;
    FLUSH_LOG;
    return 1;
  }
  LOG_INFO << "Multiview deferred rendering with PBR and antialiasing";
  if (enableDebugging)
    LOG_INFO << " : Vulkan debugging enabled";
  LOG_INFO << std::endl;

  std::vector<std::string> instanceExtensions = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
  std::vector<std::string> requestDebugLayers;
  if (enableDebugging)
    requestDebugLayers.push_back("VK_LAYER_LUNARG_standard_validation");
  pumex::ViewerTraits viewerTraits{ "Multiview Deferred PBR", instanceExtensions, requestDebugLayers, 60 };
  viewerTraits.debugReportFlags = VK_DEBUG_REPORT_ERROR_BIT_EXT;// | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT | VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT;

  std::shared_ptr<pumex::Viewer> viewer;
  try
  {
    viewer = std::make_shared<pumex::Viewer>(viewerTraits);

    std::vector<std::string> requestDeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_MULTIVIEW_EXTENSION_NAME };
    std::shared_ptr<pumex::Device> device = viewer->addDevice(0, requestDeviceExtensions);

    pumex::WindowTraits windowTraits{ 0, 100, 100, 1024, 768, useFullScreen ? pumex::WindowTraits::FULLSCREEN : pumex::WindowTraits::WINDOW, "Multiview deferred rendering with PBR and antialiasing" };
    std::shared_ptr<pumex::Window> window = pumex::Window::createWindow(windowTraits);

    pumex::SurfaceTraits surfaceTraits{ 3, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 1, VK_PRESENT_MODE_MAILBOX_KHR, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR };
    std::shared_ptr<pumex::Surface> surface = viewer->addSurface(window, device, surfaceTraits);

    std::shared_ptr<pumex::DeviceMemoryAllocator> frameBufferAllocator = std::make_shared<pumex::DeviceMemoryAllocator>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 512 * 1024 * 1024, pumex::DeviceMemoryAllocator::FIRST_FIT);

    std::vector<pumex::QueueTraits> queueTraits{ { VK_QUEUE_GRAPHICS_BIT, 0, 0.75f } };

    // images used to create and consume gbuffers in "gbuffers" and "lighting" operations are half the width of the screen, but there are two layers in each image.
    // Thanks to this little trick we don't have to change viewports and scissors
    std::shared_ptr<pumex::RenderWorkflow> workflow = std::make_shared<pumex::RenderWorkflow>("deferred_workflow", frameBufferAllocator, queueTraits);
      workflow->addResourceType("vec3_samples",  false, VK_FORMAT_R16G16B16A16_SFLOAT, SAMPLE_COUNT,          pumex::atColor,   pumex::AttachmentSize{ pumex::AttachmentSize::SurfaceDependent, glm::vec3(0.5f,1.0f,2.0f) }, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
      workflow->addResourceType("color_samples", false, VK_FORMAT_B8G8R8A8_UNORM,      SAMPLE_COUNT,          pumex::atColor,   pumex::AttachmentSize{ pumex::AttachmentSize::SurfaceDependent, glm::vec3(0.5f,1.0f,2.0f) }, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
      workflow->addResourceType("depth_samples", false, VK_FORMAT_D32_SFLOAT,          SAMPLE_COUNT,          pumex::atDepth,   pumex::AttachmentSize{ pumex::AttachmentSize::SurfaceDependent, glm::vec3(0.5f,1.0f,2.0f) }, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
      workflow->addResourceType("resolve",       false, VK_FORMAT_B8G8R8A8_UNORM,      SAMPLE_COUNT,          pumex::atColor,   pumex::AttachmentSize{ pumex::AttachmentSize::SurfaceDependent, glm::vec3(0.5f,1.0f,2.0f) }, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
      workflow->addResourceType("color",         false, VK_FORMAT_B8G8R8A8_UNORM,      VK_SAMPLE_COUNT_1_BIT, pumex::atColor,   pumex::AttachmentSize{ pumex::AttachmentSize::SurfaceDependent, glm::vec3(0.5f,1.0f,2.0f) }, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
      workflow->addResourceType("surface",       true,  VK_FORMAT_B8G8R8A8_UNORM,      VK_SAMPLE_COUNT_1_BIT, pumex::atSurface, pumex::AttachmentSize{ pumex::AttachmentSize::SurfaceDependent, glm::vec2(1.0f,1.0f) },      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    workflow->addRenderOperation("gbuffer", pumex::RenderOperation::Graphics, 0x3U, pumex::AttachmentSize{ pumex::AttachmentSize::SurfaceDependent, glm::vec3(0.5f, 1.0f, 2.0f) });
      workflow->addAttachmentOutput     ("gbuffer", "vec3_samples",  "position", VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,         pumex::loadOpClear(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)));
      workflow->addAttachmentOutput     ("gbuffer", "vec3_samples",  "normals",  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,         pumex::loadOpClear(glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)));
      workflow->addAttachmentOutput     ("gbuffer", "color_samples", "albedo",   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,         pumex::loadOpClear(glm::vec4(0.3f, 0.3f, 0.3f, 1.0f)));
      workflow->addAttachmentOutput     ("gbuffer", "color_samples", "pbr",      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,         pumex::loadOpClear(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));
      workflow->addAttachmentDepthOutput("gbuffer", "depth_samples", "depth",    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, pumex::loadOpClear(glm::vec2(1.0f, 0.0f)));

    workflow->addRenderOperation("lighting", pumex::RenderOperation::Graphics, 0x3U, pumex::AttachmentSize{ pumex::AttachmentSize::SurfaceDependent, glm::vec3(0.5f, 1.0f, 2.0f) });
      workflow->addAttachmentInput        ("lighting", "vec3_samples",  "position",         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      workflow->addAttachmentInput        ("lighting", "vec3_samples",  "normals",          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      workflow->addAttachmentInput        ("lighting", "color_samples", "albedo",           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      workflow->addAttachmentInput        ("lighting", "color_samples", "pbr",              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      workflow->addAttachmentOutput       ("lighting", "resolve",       "resolve",          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, pumex::loadOpDontCare());
      workflow->addAttachmentResolveOutput("lighting", "color",         "color", "resolve", VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, pumex::loadOpDontCare());

    // third operation copies images created in "lighting" operation to a single texture ( full width image ) and performs barrel distortion on it
    workflow->addRenderOperation("multiview", pumex::RenderOperation::Graphics, 0x0, pumex::AttachmentSize{ pumex::AttachmentSize::SurfaceDependent, glm::vec2(1.0f,1.0f) });
      workflow->addImageInput      ("multiview", "color",      "color",     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      workflow->addAttachmentOutput("multiview", "surface", "multiview", VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, pumex::loadOpDontCare());
     
    std::shared_ptr<pumex::DeviceMemoryAllocator> buffersAllocator = std::make_shared<pumex::DeviceMemoryAllocator>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1024 * 1024, pumex::DeviceMemoryAllocator::FIRST_FIT);
    // allocate 64 MB for vertex and index buffers
    std::shared_ptr<pumex::DeviceMemoryAllocator> verticesAllocator = std::make_shared<pumex::DeviceMemoryAllocator>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 64 * 1024 * 1024, pumex::DeviceMemoryAllocator::FIRST_FIT);
    // allocate 80 MB memory for textures
    std::shared_ptr<pumex::DeviceMemoryAllocator> texturesAllocator = std::make_shared<pumex::DeviceMemoryAllocator>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 80 * 1024 * 1024, pumex::DeviceMemoryAllocator::FIRST_FIT);

    std::shared_ptr<DeferredApplicationData> applicationData = std::make_shared<DeferredApplicationData>(buffersAllocator);

/*************************************/

    auto gbufferRoot = std::make_shared<pumex::Group>();
    gbufferRoot->setName("gbufferRoot");
    workflow->setRenderOperationNode("gbuffer", gbufferRoot);

    auto pipelineCache = std::make_shared<pumex::PipelineCache>();

    std::vector<pumex::DescriptorSetLayoutBinding> gbufferLayoutBindings =
    {
      { 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 1, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 2, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 3, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 4, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 5, 64, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 6, 64, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 7, 64, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 8, 64, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    auto gbufferDescriptorSetLayout = std::make_shared<pumex::DescriptorSetLayout>(gbufferLayoutBindings);

    // building gbufferPipeline layout
    auto gbufferPipelineLayout = std::make_shared<pumex::PipelineLayout>();
    gbufferPipelineLayout->descriptorSetLayouts.push_back(gbufferDescriptorSetLayout);

    std::vector<pumex::VertexSemantic> requiredSemantic = { { pumex::VertexSemantic::Position, 3 },{ pumex::VertexSemantic::Normal, 3 },{ pumex::VertexSemantic::Tangent, 3 },{ pumex::VertexSemantic::TexCoord, 3 },{ pumex::VertexSemantic::BoneIndex, 1 },{ pumex::VertexSemantic::BoneWeight, 1 } };

    auto gbufferPipeline = std::make_shared<pumex::GraphicsPipeline>(pipelineCache, gbufferPipelineLayout);
    gbufferPipeline->setName("gbufferPipeline");

    gbufferPipeline->shaderStages =
    {
      { VK_SHADER_STAGE_VERTEX_BIT, std::make_shared<pumex::ShaderModule>(viewer->getAbsoluteFilePath("shaders/multiview_gbuffers.vert.spv")), "main" },
      { VK_SHADER_STAGE_FRAGMENT_BIT, std::make_shared<pumex::ShaderModule>(viewer->getAbsoluteFilePath("shaders/multiview_gbuffers.frag.spv")), "main" }
    };
    gbufferPipeline->vertexInput =
    {
      { 0, VK_VERTEX_INPUT_RATE_VERTEX, requiredSemantic }
    };
    gbufferPipeline->blendAttachments =
    {
      { VK_FALSE, 0xF },
      { VK_FALSE, 0xF },
      { VK_FALSE, 0xF },
      { VK_FALSE, 0xF }
    };
    gbufferPipeline->rasterizationSamples = SAMPLE_COUNT;

    gbufferRoot->addChild(gbufferPipeline);

    std::vector<pumex::AssetBufferVertexSemantics> assetSemantics = { { 1, requiredSemantic } };
    std::shared_ptr<pumex::AssetBuffer> assetBuffer = std::make_shared<pumex::AssetBuffer>(assetSemantics, buffersAllocator, verticesAllocator);

    std::vector<pumex::TextureSemantic> textureSemantic = { { pumex::TextureSemantic::Diffuse, 0 },{ pumex::TextureSemantic::Specular, 1 },{ pumex::TextureSemantic::LightMap, 2 },{ pumex::TextureSemantic::Normals, 3 } };
    std::shared_ptr<pumex::TextureRegistryArrayOfTextures> textureRegistry = std::make_shared<pumex::TextureRegistryArrayOfTextures>(buffersAllocator, texturesAllocator);
    textureRegistry->setTextureSampler(0, std::make_shared<pumex::Sampler>(pumex::SamplerTraits()));
    textureRegistry->setTextureSampler(1, std::make_shared<pumex::Sampler>(pumex::SamplerTraits()));
    textureRegistry->setTextureSampler(2, std::make_shared<pumex::Sampler>(pumex::SamplerTraits()));
    textureRegistry->setTextureSampler(3, std::make_shared<pumex::Sampler>(pumex::SamplerTraits()));
    std::shared_ptr<pumex::MaterialRegistry<MaterialData>> materialRegistry = std::make_shared<pumex::MaterialRegistry<MaterialData>>(buffersAllocator);
    std::shared_ptr<pumex::MaterialSet> materialSet = std::make_shared<pumex::MaterialSet>(viewer, materialRegistry, textureRegistry, buffersAllocator, textureSemantic);

    filesystem::path sponzaFileName("sponza/sponza.dae");
    sponzaFileName = viewer->getAbsoluteFilePath(sponzaFileName);

    pumex::AssetLoaderAssimp loader;
    loader.setImportFlags(loader.getImportFlags() | aiProcess_CalcTangentSpace );
    std::shared_ptr<pumex::Asset> asset(loader.load(sponzaFileName, false, requiredSemantic));
    CHECK_LOG_THROW (asset.get() == nullptr,  "Model not loaded : " << sponzaFileName);

    pumex::BoundingBox bbox = pumex::calculateBoundingBox(*asset, 1);

    assetBuffer->registerType(MODEL_SPONZA_ID, pumex::AssetTypeDefinition(bbox));
    assetBuffer->registerObjectLOD(MODEL_SPONZA_ID, pumex::AssetLodDefinition(0.0f, 10000.0f), asset);
    materialSet->registerMaterials(MODEL_SPONZA_ID, asset);
    materialSet->endRegisterMaterials();

    auto assetBufferNode = std::make_shared<pumex::AssetBufferNode>(assetBuffer, materialSet, 1, 0);
    assetBufferNode->setName("assetBufferNode");
    gbufferPipeline->addChild(assetBufferNode);

    std::shared_ptr<pumex::AssetBufferDrawObject> modelDraw = std::make_shared<pumex::AssetBufferDrawObject>(MODEL_SPONZA_ID);
    modelDraw->setName("modelDraw");
    assetBufferNode->addChild(modelDraw);

    std::vector<glm::mat4> globalTransforms = pumex::calculateResetPosition(*asset);
    PositionData modelData;
    std::copy(begin(globalTransforms), end(globalTransforms), std::begin(modelData.bones));
    modelData.typeID = MODEL_SPONZA_ID;
    (*applicationData->positionData)  = modelData;

    auto cameraUbo             = std::make_shared<pumex::UniformBuffer>(applicationData->cameraBuffer);

    std::shared_ptr<pumex::DescriptorSet> descriptorSet = std::make_shared<pumex::DescriptorSet>(gbufferDescriptorSetLayout);
    descriptorSet->setDescriptor(0, cameraUbo);
    descriptorSet->setDescriptor(1, std::make_shared<pumex::UniformBuffer>(applicationData->positionBuffer));
    descriptorSet->setDescriptor(2, std::make_shared<pumex::StorageBuffer>(materialSet->typeDefinitionBuffer));
    descriptorSet->setDescriptor(3, std::make_shared<pumex::StorageBuffer>(materialSet->materialVariantBuffer));
    descriptorSet->setDescriptor(4, std::make_shared<pumex::StorageBuffer>(materialRegistry->materialDefinitionBuffer));
    descriptorSet->setDescriptor(5, textureRegistry->getCombinedImageSamplers(0));
    descriptorSet->setDescriptor(6, textureRegistry->getCombinedImageSamplers(1));
    descriptorSet->setDescriptor(7, textureRegistry->getCombinedImageSamplers(2));
    descriptorSet->setDescriptor(8, textureRegistry->getCombinedImageSamplers(3));
    modelDraw->setDescriptorSet(0, descriptorSet);

/**********************/

    auto lightingRoot = std::make_shared<pumex::Group>();
    lightingRoot->setName("lightingRoot");
    workflow->setRenderOperationNode("lighting", lightingRoot);

    std::shared_ptr<pumex::Asset> fullScreenTriangle = pumex::createFullScreenTriangle();

    std::vector<pumex::DescriptorSetLayoutBinding> compositeLayoutBindings =
    {
      { 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,   VK_SHADER_STAGE_FRAGMENT_BIT },
      { 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   VK_SHADER_STAGE_FRAGMENT_BIT },
      { 2, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 3, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 4, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 5, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    auto compositeDescriptorSetLayout = std::make_shared<pumex::DescriptorSetLayout>(compositeLayoutBindings);

    // building gbufferPipeline layout
    auto compositePipelineLayout = std::make_shared<pumex::PipelineLayout>();
    compositePipelineLayout->descriptorSetLayouts.push_back(compositeDescriptorSetLayout);

    auto compositePipeline = std::make_shared<pumex::GraphicsPipeline>(pipelineCache, compositePipelineLayout);
    compositePipeline->setName("compositePipeline");
    compositePipeline->shaderStages =
    {
      { VK_SHADER_STAGE_VERTEX_BIT, std::make_shared<pumex::ShaderModule>(viewer->getAbsoluteFilePath("shaders/multiview_composite.vert.spv")), "main" },
      { VK_SHADER_STAGE_FRAGMENT_BIT, std::make_shared<pumex::ShaderModule>(viewer->getAbsoluteFilePath("shaders/multiview_composite.frag.spv")), "main" }
    };
    compositePipeline->depthTestEnable = VK_FALSE;
    compositePipeline->depthWriteEnable = VK_FALSE;

    compositePipeline->vertexInput =
    {
      { 0, VK_VERTEX_INPUT_RATE_VERTEX, fullScreenTriangle->geometries[0].semantic }
    };
    compositePipeline->blendAttachments =
    {
      { VK_FALSE, 0xF }
    };
    compositePipeline->rasterizationSamples = SAMPLE_COUNT;

    lightingRoot->addChild(compositePipeline);

    std::shared_ptr<pumex::AssetNode> assetNode = std::make_shared<pumex::AssetNode>(fullScreenTriangle, verticesAllocator, 1, 0);
    assetNode->setName("fullScreenTriangleAssetNode");
    compositePipeline->addChild(assetNode);

    auto iaSampler = std::make_shared<pumex::Sampler>(pumex::SamplerTraits());

    auto compositeDescriptorSet = std::make_shared<pumex::DescriptorSet>(compositeDescriptorSetLayout);
    compositeDescriptorSet->setDescriptor(0, cameraUbo);
    compositeDescriptorSet->setDescriptor(1, std::make_shared<pumex::StorageBuffer>(applicationData->lightsBuffer));
    compositeDescriptorSet->setDescriptor(2, std::make_shared<pumex::InputAttachment>("position", iaSampler));
    compositeDescriptorSet->setDescriptor(3, std::make_shared<pumex::InputAttachment>("normals", iaSampler));
    compositeDescriptorSet->setDescriptor(4, std::make_shared<pumex::InputAttachment>("albedo", iaSampler));
    compositeDescriptorSet->setDescriptor(5, std::make_shared<pumex::InputAttachment>("pbr", iaSampler));
    assetNode->setDescriptorSet(0, compositeDescriptorSet);

/*********************/

    auto multiviewRoot = std::make_shared<pumex::Group>();
    multiviewRoot->setName("multiviewRoot");
    workflow->setRenderOperationNode("multiview", multiviewRoot);

    // Look closely at method that builds geometry for "multiview" operation ( buildMultiViewQuads() )
    // Geometry consists of two quads - each of them covers half of the screen. Z texture coordinate for the left quad is equal to 0, while Z texture coordinate for the right quad is equal to 1.
    // This way we are able to use multilayered textures created in previous operations to cover the whole screen.
    std::shared_ptr<pumex::Asset> multiviewQuads = buildMultiViewQuads();

    std::vector<pumex::DescriptorSetLayoutBinding> multiviewLayoutBindings =
    {
      { 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,    VK_SHADER_STAGE_FRAGMENT_BIT },
      { 1, 1, VK_DESCRIPTOR_TYPE_SAMPLER,          VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    auto multiviewDescriptorSetLayout = std::make_shared<pumex::DescriptorSetLayout>(multiviewLayoutBindings);

    // building gbufferPipeline layout
    auto multiviewPipelineLayout = std::make_shared<pumex::PipelineLayout>();
    multiviewPipelineLayout->descriptorSetLayouts.push_back(multiviewDescriptorSetLayout);

    auto multiviewPipeline = std::make_shared<pumex::GraphicsPipeline>(pipelineCache, multiviewPipelineLayout);
    multiviewPipeline->setName("multiviewPipeline");
    multiviewPipeline->shaderStages =
    {
      { VK_SHADER_STAGE_VERTEX_BIT, std::make_shared<pumex::ShaderModule>(viewer->getAbsoluteFilePath("shaders/multiview_display.vert.spv")), "main" },
      { VK_SHADER_STAGE_FRAGMENT_BIT, std::make_shared<pumex::ShaderModule>(viewer->getAbsoluteFilePath("shaders/multiview_display.frag.spv")), "main" }
    };
    multiviewPipeline->depthTestEnable = VK_FALSE;
    multiviewPipeline->depthWriteEnable = VK_FALSE;

    multiviewPipeline->vertexInput =
    {
      { 0, VK_VERTEX_INPUT_RATE_VERTEX, multiviewQuads->geometries[0].semantic }
    };
    multiviewPipeline->blendAttachments =
    {
      { VK_FALSE, 0xF }
    };
    multiviewPipeline->rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    multiviewRoot->addChild(multiviewPipeline);

    std::shared_ptr<pumex::AssetNode> quadsAssetNode = std::make_shared<pumex::AssetNode>(multiviewQuads, verticesAllocator, 1, 0);
    quadsAssetNode->setName("quadsAssetNode");
    multiviewPipeline->addChild(quadsAssetNode);

    auto mvSampler = std::make_shared<pumex::Sampler>(pumex::SamplerTraits());

    auto multiviewDescriptorSet = std::make_shared<pumex::DescriptorSet>(multiviewDescriptorSetLayout);
    // connect "color" attachment as pumex::SampledImage
    multiviewDescriptorSet->setDescriptor(0, std::make_shared<pumex::SampledImage>("color"));
    multiviewDescriptorSet->setDescriptor(1, mvSampler);
    quadsAssetNode->setDescriptorSet(0, multiviewDescriptorSet);

    //std::string fullFontFileName = viewer->getAbsoluteFilePath("fonts/DejaVuSans.ttf");
    //auto fontDefault = std::make_shared<pumex::Font>(fullFontFileName, glm::uvec2(1024, 1024), 24, texturesAllocator);
    //auto textDefault = std::make_shared<pumex::Text>(fontDefault, buffersAllocator);
    //textDefault->setName("textDefault");
    //applicationData->textDefault = textDefault;

    //std::vector<pumex::DescriptorSetLayoutBinding> textLayoutBindings =
    //{
    //  { 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_GEOMETRY_BIT },
    //  { 1, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT }
    //};
    //auto textDescriptorSetLayout = std::make_shared<pumex::DescriptorSetLayout>(textLayoutBindings);
    //// building pipeline layout
    //auto textPipelineLayout = std::make_shared<pumex::PipelineLayout>();
    //textPipelineLayout->descriptorSetLayouts.push_back(textDescriptorSetLayout);
    //auto textPipeline = std::make_shared<pumex::GraphicsPipeline>(pipelineCache, textPipelineLayout);
    //textPipeline->setName("textPipeline");
    //textPipeline->vertexInput =
    //{
    //  { 0, VK_VERTEX_INPUT_RATE_VERTEX, textDefault->textVertexSemantic }
    //};
    //textPipeline->topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    //textPipeline->blendAttachments =
    //{
    //  { VK_TRUE, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    //  VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
    //  VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD }
    //};
    //textPipeline->depthTestEnable = VK_FALSE;
    //textPipeline->depthWriteEnable = VK_FALSE;
    //textPipeline->shaderStages =
    //{
    //  { VK_SHADER_STAGE_VERTEX_BIT,   std::make_shared<pumex::ShaderModule>(viewer->getAbsoluteFilePath("shaders/text_draw.vert.spv")), "main" },
    //  { VK_SHADER_STAGE_GEOMETRY_BIT, std::make_shared<pumex::ShaderModule>(viewer->getAbsoluteFilePath("shaders/text_draw.geom.spv")), "main" },
    //  { VK_SHADER_STAGE_FRAGMENT_BIT, std::make_shared<pumex::ShaderModule>(viewer->getAbsoluteFilePath("shaders/text_draw.frag.spv")), "main" }
    //};
    //textPipeline->rasterizationSamples = SAMPLE_COUNT;

    //lightingRoot->addChild(textPipeline);
    //textPipeline->addChild(textDefault);

    //auto fontImageView = std::make_shared<pumex::ImageView>(fontDefault->fontMemoryImage, fontDefault->fontMemoryImage->getFullImageRange(), VK_IMAGE_VIEW_TYPE_2D);
    //auto fontSampler = std::make_shared<pumex::Sampler>(pumex::SamplerTraits());

    //auto textCameraUbo = std::make_shared<pumex::UniformBuffer>(applicationData->textCameraBuffer);

    //auto textDescriptorSet = std::make_shared<pumex::DescriptorSet>(textDescriptorSetLayout);
    //textDescriptorSet->setDescriptor(0, textCameraUbo);
    //textDescriptorSet->setDescriptor(1, std::make_shared<pumex::CombinedImageSampler>(fontImageView, fontSampler));
    //textDefault->setDescriptorSet(0, textDescriptorSet);

    // connect workflow to a surface
    std::shared_ptr<pumex::SingleQueueWorkflowCompiler> workflowCompiler = std::make_shared<pumex::SingleQueueWorkflowCompiler>();
    surface->setRenderWorkflow(workflow, workflowCompiler);

    // build simple update graph
    tbb::flow::continue_node< tbb::flow::continue_msg > update(viewer->updateGraph, [=](tbb::flow::continue_msg)
    {
      applicationData->processInput(surface);
      applicationData->update(pumex::inSeconds(viewer->getUpdateTime() - viewer->getApplicationStartTime()), pumex::inSeconds(viewer->getUpdateDuration()));
    });
    tbb::flow::make_edge(viewer->opStartUpdateGraph, update);
    tbb::flow::make_edge(update, viewer->opEndUpdateGraph);

    // set render callbacks to application data
    viewer->setEventRenderStart(std::bind(&DeferredApplicationData::prepareModelForRendering, applicationData, std::placeholders::_1, assetBuffer, MODEL_SPONZA_ID));
    surface->setEventSurfaceRenderStart(std::bind(&DeferredApplicationData::prepareCameraForRendering, applicationData, std::placeholders::_1));

    viewer->run();
  }
  catch (const std::exception& e)
  {
#if defined(_DEBUG) && defined(_WIN32)
    OutputDebugStringA("Exception thrown : ");
    OutputDebugStringA(e.what());
    OutputDebugStringA("\n");
#endif
    LOG_ERROR << "Exception thrown : " << e.what() << std::endl;
  }
  catch (...)
  {
#if defined(_DEBUG) && defined(_WIN32)
    OutputDebugStringA("Unknown error\n");
#endif
    LOG_ERROR << "Unknown error" << std::endl;
  }
  viewer->cleanup();
  FLUSH_LOG;
  return 0;
}