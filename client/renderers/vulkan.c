/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "lg-renderer.h"
#include <vulkan/vulkan.h>
#include <SDL2/SDL_syswm.h>

#include "debug.h"

#define VERTEX_SHADER \
"#version 450"                                                   "\n" \
""                                                               "\n" \
"layout (location = 0) out vec2 outUV;"                          "\n" \
""                                                               "\n" \
"void main()"                                                    "\n" \
"{"                                                              "\n" \
"  outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);" "\n" \
"  gl_Position = vec4(outUV * 2.0f + -1.0f, 0.0f, 1.0f);"        "\n" \
"}"

typedef struct QueueIndicies
{
  int graphics;
  int present;
}
QueueIndicies;

struct LGR_Vulkan
{
  LG_RendererParams params;
  LG_RendererFormat format;
  bool              configured;

  VkInstance        instance;
  VkSurfaceKHR      surface;
  VkDevice          device;
  VkPresentModeKHR  presentMode;

  VkPhysicalDevice  physicalDevice;
  QueueIndicies     queues;
  VkQueue           graphics_q;
  VkQueue           present_q;
  bool              chainCreated;

  VkExtent2D        extent;
  VkSwapchainKHR    swapChain;
  uint32_t          imageCount;
  VkImage         * images;
  VkImageView     * views;
  VkRenderPass      renderPass;
  VkPipelineLayout  pipelineLayout;
  VkPipeline        pipeline;
  uint32_t          framebufferCount;
  VkFramebuffer   * framebuffers;
  VkCommandPool     commandPool;
  VkCommandBuffer * commandBuffers;
  VkSemaphore       semImageAvailable;
  VkSemaphore       semRenderFinished;
};

//=============================================================================
//
// Forwards for top level vulkan initialization and teardown
//
//=============================================================================
static bool create_instance      (struct LGR_Vulkan * this);
static bool create_surface       (struct LGR_Vulkan * this, SDL_Window * window);
static bool pick_physical_device (struct LGR_Vulkan * this);
static bool create_logical_device(struct LGR_Vulkan * this);
static bool create_chain         (struct LGR_Vulkan * this, int w, int h);
static void delete_chain         (struct LGR_Vulkan * this);

const char * lgr_vulkan_get_name()
{
  return "Vulkan";
}

bool lgr_vulkan_initialize(void ** opaque, const LG_RendererParams params, Uint32 * sdlFlags)
{
  // create our local storage
  *opaque = malloc(sizeof(struct LGR_Vulkan));
  if (!*opaque)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(struct LGR_Vulkan));
    return false;
  }
  memset(*opaque, 0, sizeof(struct LGR_Vulkan));

  struct LGR_Vulkan * this = (struct LGR_Vulkan *)*opaque;
  memcpy(&this->params, &params, sizeof(LG_RendererParams));

  return
    create_instance(this);
}

bool lgr_vulkan_configure(void * opaque, SDL_Window *window, const LG_RendererFormat format)
{
  struct LGR_Vulkan * this = (struct LGR_Vulkan *)opaque;
  if (!this)
    return false;

  if (this->configured)
  {
    DEBUG_ERROR("Already configured, call deconfigure first");
    return false;
  }

  memcpy(&this->format, &format, sizeof(LG_RendererFormat));
  int x, y;
  SDL_GetWindowSize(window, &x, &y);

  this->configured =
    create_surface       (this, window) &&
    pick_physical_device (this) &&
    create_logical_device(this) &&
    create_chain         (this, x, y);

  return this->configured;
}

void lgr_vulkan_deconfigure(void * opaque)
{
  struct LGR_Vulkan * this = (struct LGR_Vulkan *)opaque;
  if (!this)
    return;

  delete_chain(this);

  if (this->device)
  {
    vkDestroyDevice(this->device, NULL);
    this->device = NULL;
  }

  if (this->surface)
  {
    vkDestroySurfaceKHR(this->instance, this->surface, NULL);
    this->surface = NULL;
  }

  this->configured = false;
}

void lgr_vulkan_deinitialize(void * opaque)
{
  struct LGR_Vulkan * this = (struct LGR_Vulkan *)opaque;
  if (!this)
    return;

  delete_chain(this);

  if (this->configured)
    lgr_vulkan_deconfigure(opaque);

  if (this->instance)
    vkDestroyInstance(this->instance, NULL);

  free(this);
}

bool lgr_vulkan_is_compatible(void * opaque, const LG_RendererFormat format)
{
  struct LGR_Vulkan * this = (struct LGR_Vulkan *)opaque;
  if (!this || !this->configured)
    return false;

  return (memcmp(&this->format, &format, sizeof(LG_RendererFormat)) == 0);
}

void lgr_vulkan_on_resize(void * opaque, const int width, const int height, const LG_RendererRect destRect)
{
  struct LGR_Vulkan * this = (struct LGR_Vulkan *)opaque;
  if (!this || !this->configured)
    return;

  if (this->extent.width != width || this->extent.height != height)
  {
    delete_chain(this);
    create_chain(this, width, height);
  }
}

bool lgr_vulkan_on_mouse_shape(void * opaque, const LG_RendererCursor cursor, const int width, const int height, const int pitch, const uint8_t * data)
{
  struct LGR_Vulkan * this = (struct LGR_Vulkan *)opaque;
  if (!this || !this->configured)
    return false;

  return true;
}

bool lgr_vulkan_on_mouse_event(void * opaque, const bool visible , const int x, const int y)
{
  struct LGR_Vulkan * this = (struct LGR_Vulkan *)opaque;
  if (!this || !this->configured)
    return false;

  return true;
}

bool lgr_vulkan_on_frame_event(void * opaque, const uint8_t * data)
{
  struct LGR_Vulkan * this = (struct LGR_Vulkan *)opaque;
  if (!this || !this->configured)
    return false;

  return true;
}

bool lgr_vulkan_render(void * opaque)
{
  struct LGR_Vulkan * this = (struct LGR_Vulkan *)opaque;
  if (!this || !this->configured)
    return false;

  uint32_t imageIndex;
  bool ok = false;
  for(int retry = 0; retry < 2; ++retry)
  {
    vkQueueWaitIdle(this->present_q);
    switch(vkAcquireNextImageKHR(this->device, this->swapChain, 1e6, this->semImageAvailable, VK_NULL_HANDLE, &imageIndex))
    {
      case VK_SUCCESS:
        ok = true;
        break;

      case VK_TIMEOUT:
        DEBUG_ERROR("Acquire next image timeout");
        return false;

      case VK_NOT_READY:
        DEBUG_ERROR("Acquire next image not ready");
        return false;

      case VK_SUBOPTIMAL_KHR:
        DEBUG_INFO("Suboptimal, recreating the chain");
        delete_chain(this);
        if (!create_chain(this, this->extent.width, this->extent.height))
          return false;
        break;

      default:
        DEBUG_ERROR("Acquire next image failed");
        return false;
    }

    if (ok)
      break;
  }

  if (!ok)
  {
    DEBUG_ERROR("Retry limit exceeded");
    return false;
  }

  VkSemaphore          waitSemaphores[] = { this->semImageAvailable };
  VkSemaphore          doneSemaphores[] = { this->semRenderFinished };
  VkPipelineStageFlags waitStages[]     = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
  VkSubmitInfo         submitInfo       = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .waitSemaphoreCount   = 1,
    .pWaitSemaphores      = waitSemaphores,
    .pWaitDstStageMask    = waitStages,
    .commandBufferCount   = 1,
    .pCommandBuffers      = &this->commandBuffers[imageIndex],
    .signalSemaphoreCount = 1,
    .pSignalSemaphores    = doneSemaphores
  };

  if (vkQueueSubmit(this->graphics_q, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to submit the draw command buffer");
  }

  VkSwapchainKHR   swapChains[] = { this->swapChain };
  VkPresentInfoKHR presentInfo  =
  {
    .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
    .waitSemaphoreCount = 1,
    .pWaitSemaphores    = doneSemaphores,
    .swapchainCount     = 1,
    .pSwapchains        = swapChains,
    .pImageIndices      = &imageIndex
  };

  if (vkQueuePresentKHR(this->present_q, &presentInfo) != VK_SUCCESS)
  {
    DEBUG_ERROR("Queue present failed");
    return false;
  }

  return true;
}

const LG_Renderer LGR_Vulkan =
{
  .get_name       = lgr_vulkan_get_name,
  .initialize     = lgr_vulkan_initialize,
  .configure      = lgr_vulkan_configure,
  .deconfigure    = lgr_vulkan_deconfigure,
  .deinitialize   = lgr_vulkan_deinitialize,
  .is_compatible  = lgr_vulkan_is_compatible,
  .on_resize      = lgr_vulkan_on_resize,
  .on_mouse_shape = lgr_vulkan_on_mouse_shape,
  .on_mouse_event = lgr_vulkan_on_mouse_event,
  .on_frame_event = lgr_vulkan_on_frame_event,
  .render         = lgr_vulkan_render
};

// vulkan internals

static bool create_instance(struct LGR_Vulkan * this)
{
  VkApplicationInfo appInfo =
  {
    .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName   = "Looking Glass",
    .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
    .pEngineName        = "No Engine",
    .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
    .apiVersion         = VK_API_VERSION_1_0
  };

  const char * extensionNames[2] =
  {
    "VK_KHR_surface",
    "VK_KHR_xlib_surface",
  };

  const char * layerNames[0] =
  {
  };

  VkInstanceCreateInfo createInfo =
  {
    .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo        = &appInfo,
    .enabledExtensionCount   = 2,
    .ppEnabledExtensionNames = extensionNames,
    .enabledLayerCount       = 0,
    .ppEnabledLayerNames     = layerNames
  };

  if (vkCreateInstance(&createInfo, NULL, &this->instance) != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create the instance");
    return false;
  }

  return true;
}

static bool create_surface(struct LGR_Vulkan * this, SDL_Window * window)
{
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(window, &info))
  {
    DEBUG_ERROR("Failed to obtain SDL window information");
    return false;
  }

  switch(info.subsystem)
  {
#if defined(VK_USE_PLATFORM_XLIB_KHR)
    case SDL_SYSWM_X11:
      {
        VkXlibSurfaceCreateInfoKHR createInfo;
        memset(&createInfo, 0, sizeof(VkXlibSurfaceCreateInfoKHR));
        createInfo.sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
        createInfo.dpy    = info.info.x11.display;
        createInfo.window = info.info.x11.window;

        if (vkCreateXlibSurfaceKHR(this->instance, &createInfo, NULL, &this->surface) != VK_SUCCESS)
        {
          DEBUG_ERROR("Failed to create Xlib Surface");
          return false;
        }
        break;
      }
#endif
#if 0
    case SDL_SYSWM_MIR:
      break;

    case SDL_SYSWM_WAYLAND:
      break;

    case SDL_SYSWM_ANDROID:
      break;
#endif
    default:
      DEBUG_ERROR("Unsupported window subsystem");
      return false;
  }

  return true;
}

static bool pick_physical_device(struct LGR_Vulkan * this)
{
  uint32_t deviceCount;
  vkEnumeratePhysicalDevices(this->instance, &deviceCount, NULL);
  if (!deviceCount)
  {
    DEBUG_ERROR("failed to find a GPU with Vulkan support!");
    return false;
  }

  // enumerate the devices and choose one that suits best
  VkPhysicalDevice physicalDevices[deviceCount];
  int32_t          scores[deviceCount];
  int32_t          maxScore = 0;

  vkEnumeratePhysicalDevices(this->instance, &deviceCount, physicalDevices);
  for(int i = 0; i < deviceCount; ++i)
  {
    VkPhysicalDevice           pd = physicalDevices[i];
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures   features;
    scores[i] = 0;

    vkGetPhysicalDeviceProperties(pd, &properties);
    vkGetPhysicalDeviceFeatures  (pd, &features  );

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &queueFamilyCount, NULL);
    if (!queueFamilyCount)
      continue;

    VkQueueFamilyProperties queueFamilies[queueFamilyCount];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &queueFamilyCount, queueFamilies);

    // ensure the device has a graphics and present queues
    bool complete = false;
    this->queues.graphics = -1;
    this->queues.present  = -1;
    for(int i = 0; i < queueFamilyCount; ++i)
    {
      const VkQueueFamilyProperties f = queueFamilies[i];
      if (this->queues.graphics == -1 && f.queueFlags & VK_QUEUE_GRAPHICS_BIT)
        this->queues.graphics = i;

      if (this->queues.present == -1)
      {
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, this->surface, &presentSupport);

        if (presentSupport)
          this->queues.present = i;
      }

      if (this->queues.graphics != -1 && this->queues.present != -1)
      {
        complete = true;
        break;
      }
    }

    if (!complete)
      continue;

    // ensure the device supports swapChain
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &extensionCount, NULL);
    if (!extensionCount)
      continue;

    complete = false;
    VkExtensionProperties extensions[extensionCount];
    vkEnumerateDeviceExtensionProperties(pd, NULL, &extensionCount, extensions);
    for(int i = 0; i < extensionCount; ++i)
      if (strncmp(
        extensions[i].extensionName,
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        sizeof(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) == 0)
      {
        complete = true;
        break;
      }

    if (!complete)
      continue;

    // ensure the device supports the required format
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR (pd, this->surface, &formatCount, NULL);
    if (!formatCount)
      continue;

    VkSurfaceFormatKHR formats[formatCount];
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, this->surface, &formatCount, formats);

    if (formatCount != 1 || formats[0].format != VK_FORMAT_UNDEFINED)
    {
      complete = false;
      for(int i = 0; i < formatCount; ++i)
      {
        const VkSurfaceFormatKHR format = formats[i];
        if (
          format.format     == VK_FORMAT_B8G8R8A8_UNORM          &&
          format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
        )
        {
          complete = true;
          break;
        }
      }

      if (!complete)
        continue;
    }

    // score the device
    switch(properties.deviceType)
    {
      case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        scores[i] += 100;
        break;

      case VK_PHYSICAL_DEVICE_TYPE_CPU:
        scores[i] += 200;
        break;

      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        scores[i] += 300;
        break;

      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        scores[i] += 400;
        break;

      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        scores[i] += 500;
        break;

      // invalid deviceType
      default:
        scores[i] -= 100;
        break;
    }

    scores[i] += features.logicOp ? 10 : 0;
    scores[i] += properties.limits.maxImageDimension2D / 1000;

    if (scores[i] > maxScore)
      maxScore = scores[i];
  }

  typedef struct
  {
    const char       * name;
    VkPresentModeKHR   mode;
  }
  PresentMode;

  static const PresentMode PresentModePrio[] =
  {
    { .name = "Mailbox"     , .mode = VK_PRESENT_MODE_MAILBOX_KHR      },
    { .name = "FIFO Relaxed", .mode = VK_PRESENT_MODE_FIFO_RELAXED_KHR },
    { .name = "Immediate"   , .mode = VK_PRESENT_MODE_IMMEDIATE_KHR    },
    { .name = "FIFO"        , .mode = VK_PRESENT_MODE_FIFO_KHR         },
    { .name = NULL } // sentinal
  };

  this->physicalDevice = VK_NULL_HANDLE;
  for(int i = 0; i < deviceCount; ++i)
    if (scores[i] == maxScore)
    {
      this->physicalDevice = physicalDevices[i];
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(this->physicalDevice, &properties);

      DEBUG_INFO("Score         : %d"  , scores[i]                            );
      DEBUG_INFO("API Version   : 0x%x", properties.apiVersion                );
      DEBUG_INFO("Driver Version: 0x%x", properties.driverVersion             );
      DEBUG_INFO("Vendor ID     : 0x%x", properties.vendorID                  );
      DEBUG_INFO("Device ID     : 0x%x", properties.deviceID                  );
      DEBUG_INFO("Device Name   : %s"  , properties.deviceName                );
      DEBUG_INFO("maxImageDim2D : %d"  , properties.limits.maxImageDimension2D);

      // get the present modes
      uint32_t presentModeCount;
      vkGetPhysicalDeviceSurfacePresentModesKHR(this->physicalDevice, this->surface, &presentModeCount, NULL);
      if (!presentModeCount)
        continue;
      VkPresentModeKHR presentModes[presentModeCount];
      vkGetPhysicalDeviceSurfacePresentModesKHR(this->physicalDevice, this->surface, &presentModeCount, presentModes);

      // find the best match
      bool found = false;
      for(int i = 0; PresentModePrio[i].name != NULL; ++i)
      {
        for(int c = 0; c < presentModeCount; ++c)
          if (presentModes[c] == PresentModePrio[i].mode)
          {
            found = true;
            this->presentMode = PresentModePrio[i].mode;
            DEBUG_INFO("Present Mode  : %s", PresentModePrio[i].name);
            break;
          }

        if(found)
          break;
      }

      if (!found)
      {
        DEBUG_ERROR("Failed to select a present mode");
        return false;
      }

      break;
    }

  if (this->physicalDevice == VK_NULL_HANDLE)
  {
    DEBUG_ERROR("Suitable GPU not found");
    return false;
  }

  return true;
}

static bool create_logical_device(struct LGR_Vulkan * this)
{
  float priority = 1.0f;
  VkDeviceQueueCreateInfo queueInfo[2] =
  {
    {
      .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = this->queues.graphics,
      .queueCount       = 1,
      .pQueuePriorities = &priority
    },
    {
      .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = this->queues.present,
      .queueCount       = 1,
      .pQueuePriorities = &priority
    }
  };

  VkPhysicalDeviceFeatures features;
  memset(&features, 0, sizeof(VkPhysicalDeviceFeatures));

  const char * extensions[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

  VkDeviceCreateInfo createInfo =
  {
    .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .pQueueCreateInfos       = queueInfo,
    .queueCreateInfoCount    = 2,
    .pEnabledFeatures        = &features,
    .enabledExtensionCount   = 1,
    .ppEnabledExtensionNames = extensions
  };

  if (vkCreateDevice(this->physicalDevice, &createInfo, NULL, &this->device) != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create the logical device");
    return false;
  }

  vkGetDeviceQueue(this->device, this->queues.graphics, 0, &this->graphics_q);
  vkGetDeviceQueue(this->device, this->queues.present , 0, &this->present_q );
  return true;
}

//=============================================================================
//
// Lower recreatable swapChain level
//
//=============================================================================

static bool create_swap_chain      (struct LGR_Vulkan * this, int w, int h);
static bool create_image_views    (struct LGR_Vulkan * this);
static bool create_render_pass    (struct LGR_Vulkan * this);
static bool create_pipeline       (struct LGR_Vulkan * this);
static bool create_framebuffers   (struct LGR_Vulkan * this);
static bool create_command_buffers(struct LGR_Vulkan * this);
static bool create_semaphores     (struct LGR_Vulkan * this);

static bool create_chain(struct LGR_Vulkan * this, int w, int h)
{
  this->chainCreated =
    create_swap_chain     (this, w, h) &&
    create_image_views    (this) &&
    create_render_pass    (this) &&
    create_pipeline       (this) &&
    create_framebuffers   (this) &&
    create_command_buffers(this) &&
    create_semaphores     (this);

  return this->chainCreated;
}

static void delete_chain(struct LGR_Vulkan * this)
{
  if (!this->chainCreated)
    return;

  vkDeviceWaitIdle(this->device);

  if (this->semRenderFinished)
  {
    vkDestroySemaphore(this->device, this->semRenderFinished, NULL);
    this->semRenderFinished = NULL;
  }

  if (this->semImageAvailable)
  {
    vkDestroySemaphore(this->device, this->semImageAvailable, NULL);
    this->semImageAvailable = NULL;
  }

  if (this->commandBuffers)
  {
    free(this->commandBuffers);
    this->commandBuffers = NULL;
  }

  if (this->commandPool)
  {
    vkDestroyCommandPool(this->device, this->commandPool, NULL);
    this->commandPool = NULL;
  }

  if (this->framebuffers)
  {
    for(uint32_t i = 0; i < this->framebufferCount; ++i)
      vkDestroyFramebuffer(this->device, this->framebuffers[i], NULL);
    free(this->framebuffers);
    this->framebuffers     = NULL;
    this->framebufferCount = 0;
  }

  if (this->pipeline)
  {
    vkDestroyPipeline(this->device, this->pipeline, NULL);
    this->pipeline = NULL;
  }

  if (this->pipelineLayout)
  {
    vkDestroyPipelineLayout(this->device, this->pipelineLayout, NULL);
    this->pipelineLayout = NULL;
  }

  if (this->renderPass)
  {
    vkDestroyRenderPass(this->device, this->renderPass, NULL);
    this->renderPass = NULL;
  }

  if (this->views)
  {
    for(int i = 0; i < this->imageCount; ++i)
      vkDestroyImageView(this->device, this->views[i], NULL);
    free(this->views);
    this->views = NULL;
  }

  if (this->images)
  {
    free(this->images);
    this->images = NULL;
  }

  if (this->swapChain)
  {
    vkDestroySwapchainKHR(this->device, this->swapChain, NULL);
    this->swapChain = NULL;
  }

  this->chainCreated = false;
}

static bool create_swap_chain(struct LGR_Vulkan * this, int w, int h)
{
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(this->physicalDevice, this->surface, &caps);

  this->extent.width  = w;
  this->extent.height = h;

        if (this->extent.width  > caps.maxImageExtent.width ) this->extent.width  = caps.maxImageExtent.width;
  else {if (this->extent.width  < caps.minImageExtent.width ) this->extent.width  = caps.minImageExtent.width;}

        if (this->extent.height > caps.maxImageExtent.height) this->extent.height = caps.maxImageExtent.height;
  else {if (this->extent.height < caps.minImageExtent.height) this->extent.height = caps.minImageExtent.height;}
  DEBUG_INFO("Buffer Extent : %ux%u", this->extent.width, this->extent.height);

  VkSwapchainCreateInfoKHR createInfo =
  {
    .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
    .surface          = this->surface,
    .minImageCount    = caps.minImageCount,
    .imageFormat      = VK_FORMAT_B8G8R8A8_UNORM,
    .imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
    .imageExtent      = this->extent,
    .imageArrayLayers = 1,
    .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    .preTransform     = caps.currentTransform,
    .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
    .presentMode      = this->presentMode,
    .clipped          = VK_TRUE,
    .oldSwapchain     = VK_NULL_HANDLE
  };

  uint32_t queueFamily[2] = { this->queues.graphics, this->queues.present };
  if (queueFamily[0] == queueFamily[1])
  {
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }
  else
  {
    createInfo.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices   = queueFamily;
  }

  if (vkCreateSwapchainKHR(this->device, &createInfo, NULL, &this->swapChain) != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create the swap chain");
    return false;
  }

  vkGetSwapchainImagesKHR(this->device, this->swapChain, &this->imageCount, NULL);
  if (!this->imageCount)
  {
    DEBUG_ERROR("No swapChain images");
    return false;
  }

  this->images = (VkImage *)malloc(sizeof(VkImage) * this->imageCount);
  vkGetSwapchainImagesKHR(this->device, this->swapChain, &this->imageCount, this->images);
  DEBUG_INFO("Images        : %d", this->imageCount);

  return true;
}

static bool create_image_views(struct LGR_Vulkan * this)
{
  VkImageViewCreateInfo createInfo =
  {
    .sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .viewType                        = VK_IMAGE_VIEW_TYPE_2D,
    .format                          = VK_FORMAT_B8G8R8A8_UNORM,
    .components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY,
    .components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY,
    .components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY,
    .components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY,
    .subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
    .subresourceRange.baseMipLevel   = 0,
    .subresourceRange.levelCount     = 1,
    .subresourceRange.baseArrayLayer = 0,
    .subresourceRange.layerCount     = 1,
  };

  this->views = (VkImageView *)malloc(sizeof(VkImageView) * this->imageCount);
  for(uint32_t i = 0; i < this->imageCount; ++i)
  {
    createInfo.image = this->images[i];
    if (vkCreateImageView(this->device, &createInfo, NULL, &this->views[i]) != VK_SUCCESS)
    {
      DEBUG_ERROR("failed to create image views");
      for(uint32_t a = 0; a < i; ++a)
        vkDestroyImageView(this->device, this->views[a], NULL);
      free(this->views);
      this->views = NULL;
      return false;
    }
  }

  return true;
}

static bool create_render_pass(struct LGR_Vulkan * this)
{
  VkAttachmentDescription colorAttachment =
  {
    .format         = VK_FORMAT_B8G8R8A8_UNORM,
    .samples        = VK_SAMPLE_COUNT_1_BIT,
    .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
    .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
    .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
    .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
  };

  VkAttachmentReference colorAttachmentRef =
  {
    .attachment = 0,
    .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
  };

  VkSubpassDescription subpass =
  {
    .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
    .colorAttachmentCount = 1,
    .pColorAttachments    = &colorAttachmentRef
  };

  VkRenderPassCreateInfo createInfo =
  {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments    = &colorAttachment,
    .subpassCount    = 1,
    .pSubpasses      = &subpass
  };

  if (vkCreateRenderPass(this->device, &createInfo, NULL, &this->renderPass) != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create the render pass");
    return false;
  }

  return true;
}

static inline bool create_shader_module(struct LGR_Vulkan * this, const char * code, const int codeSize, VkShaderModule * shaderModule)
{
  VkShaderModuleCreateInfo createInfo =
  {
    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = codeSize,
    .pCode    = (const uint32_t*)code
  };

  return
    vkCreateShaderModule(this->device, &createInfo, NULL, shaderModule) == VK_SUCCESS;
}

static bool create_pipeline(struct LGR_Vulkan * this)
{
  VkPipelineLayoutCreateInfo pipelineLayoutInfo =
  {
    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount         = 0,
    .pushConstantRangeCount = 0
  };

  if (vkCreatePipelineLayout(this->device, &pipelineLayoutInfo, NULL, &this->pipelineLayout) != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create the pipeline layout");
    return false;
  }

  VkShaderModule vertexShader;
  if (!create_shader_module(this, VERTEX_SHADER, sizeof(VERTEX_SHADER), &vertexShader))
  {
    DEBUG_ERROR("Failed to create the vertex shader");
    return false;
  }

  VkPipelineShaderStageCreateInfo shaderStages[1] =
  {
    {
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = VK_SHADER_STAGE_VERTEX_BIT,
      .module = vertexShader,
      .pName  = "main"
    }
  };

  VkPipelineVertexInputStateCreateInfo vertexInputState =
  {
    .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount   = 0,
    .vertexAttributeDescriptionCount = 0
  };

  VkPipelineInputAssemblyStateCreateInfo inputAssembly =
  {
    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .primitiveRestartEnable = VK_FALSE
  };

  VkViewport viewport =
  {
    .x        = 0.0f,
    .y        = 0.0f,
    .width    = (float)this->extent.width,
    .height   = (float)this->extent.height,
    .minDepth = 0.0f,
    .maxDepth = 1.0f
  };

  VkRect2D scissor =
  {
    .offset = {0, 0},
    .extent = this->extent
  };

  VkPipelineViewportStateCreateInfo viewportState =
  {
    .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount = 1,
    .pViewports    = &viewport,
    .scissorCount  = 1,
    .pScissors     = &scissor
  };

  VkPipelineRasterizationStateCreateInfo rasterizer =
  {
    .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .depthClampEnable        = VK_FALSE,
    .rasterizerDiscardEnable = VK_FALSE,
    .polygonMode             = VK_POLYGON_MODE_FILL,
    .lineWidth               = 1.0f,
    .cullMode                = VK_CULL_MODE_FRONT_BIT,
    .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
    .depthBiasEnable         = VK_FALSE
  };

  VkPipelineMultisampleStateCreateInfo multisampling =
  {
    .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .sampleShadingEnable  = VK_FALSE,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
  };

  VkPipelineColorBlendAttachmentState colorBlendAttachment =
  {
    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    .blendEnable    = VK_FALSE
  };

  VkPipelineColorBlendStateCreateInfo colorBlending =
  {
    .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    .logicOpEnable   = VK_FALSE,
    .logicOp         = VK_LOGIC_OP_COPY,
    .attachmentCount = 1,
    .pAttachments    = &colorBlendAttachment,
    .blendConstants  = {0.0f, 0.0f, 0.0f, 0.0f},
  };

  VkGraphicsPipelineCreateInfo createInfo =
  {
    .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
    .stageCount          = 1,
    .pStages             = shaderStages,
    .pVertexInputState   = &vertexInputState,
    .pInputAssemblyState = &inputAssembly,
    .pViewportState      = &viewportState,
    .pRasterizationState = &rasterizer,
    .pMultisampleState   = &multisampling,
    .pColorBlendState    = &colorBlending,
    .layout              = this->pipelineLayout,
    .renderPass          = this->renderPass,
    .subpass             = 0,
    .basePipelineHandle  = VK_NULL_HANDLE
  };

  if (vkCreateGraphicsPipelines(this->device, VK_NULL_HANDLE, 1, &createInfo, NULL, &this->pipeline) != VK_SUCCESS)
  {
    vkDestroyShaderModule(this->device, vertexShader, NULL);
    DEBUG_ERROR("Failed to create the graphics pipeline");
    return false;
  }

  vkDestroyShaderModule(this->device, vertexShader, NULL);
  return true;
}

static bool create_framebuffers(struct LGR_Vulkan * this)
{
  VkFramebufferCreateInfo createInfo =
  {
    .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
    .renderPass      = this->renderPass,
    .attachmentCount = 1,
    .width           = this->extent.width,
    .height          = this->extent.height,
    .layers          = 1
  };

  this->framebuffers = (VkFramebuffer *)malloc(sizeof(VkFramebuffer) * this->imageCount);
  for(uint32_t i = 0; i < this->imageCount; ++i)
  {
    createInfo.pAttachments = &this->views[i];
    if (vkCreateFramebuffer(this->device, &createInfo, NULL, &this->framebuffers[i]) != VK_SUCCESS)
    {
      DEBUG_ERROR("Failed to create a framebuffer");
      return false;
    }
    ++this->framebufferCount;
  }

  return true;
}

static bool create_command_buffers(struct LGR_Vulkan * this)
{
  VkCommandPoolCreateInfo poolInfo =
  {
    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = this->queues.graphics,
    .flags            = 0
  };

  if (vkCreateCommandPool(this->device, &poolInfo, NULL, &this->commandPool) != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create the command pool");
    return false;
  }

  this->commandBuffers = (VkCommandBuffer *)malloc(sizeof(VkCommandBuffer) * this->imageCount);
  VkCommandBufferAllocateInfo bufferInfo =
  {
    .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool        = this->commandPool,
    .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = this->imageCount
  };

  if (vkAllocateCommandBuffers(this->device, &bufferInfo, this->commandBuffers) != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to allocate the command buffers");
    return false;
  }

  VkCommandBufferBeginInfo beginInfo =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT
  };

  VkClearValue clearColor = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };

  VkRenderPassBeginInfo renderPassInfo =
  {
    .sType      = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
    .renderPass = this->renderPass,
    .renderArea =
    {
      .offset = {0, 0},
      .extent = this->extent
    },
    .clearValueCount = 1,
    .pClearValues    = &clearColor
  };

  for(uint32_t i = 0; i < this->imageCount; ++i)
  {
    vkBeginCommandBuffer(this->commandBuffers[i], &beginInfo);

      renderPassInfo.framebuffer = this->framebuffers[i];
      vkCmdBeginRenderPass(this->commandBuffers[i], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(this->commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, this->pipeline);
        vkCmdDraw(this->commandBuffers[i], 3, 1, 0, 0);

      vkCmdEndRenderPass(this->commandBuffers[i]);

    if (vkEndCommandBuffer(this->commandBuffers[i]) != VK_SUCCESS)
    {
      DEBUG_ERROR("Failed to record to the command buffer");
      return false;
    }
  }

  return true;
}

static bool create_semaphores(struct LGR_Vulkan * this)
{
  VkSemaphoreCreateInfo createInfo =
  {
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
  };

  if (
    (vkCreateSemaphore(this->device, &createInfo, NULL, &this->semImageAvailable) != VK_SUCCESS) ||
    (vkCreateSemaphore(this->device, &createInfo, NULL, &this->semRenderFinished) != VK_SUCCESS)
  ) {
    DEBUG_ERROR("Failed to create the semaphores");
    return false;
  }

  return true;
}