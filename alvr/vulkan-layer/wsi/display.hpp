#pragma once

#include <atomic>
#include <vulkan/vulkan.h>
#include <thread>

namespace layer
{
class device_private_data;
}

namespace wsi {

class display {
  public:
    display(layer::device_private_data& device_data);
    ~display();

    VkFence get_vsync_fence();
    VkFence peek_vsync_fence() { return vsync_fence;};

    std::atomic<uint64_t> m_vsync_count{0};

  private:
    std::atomic_bool m_thread_running{false};
    std::atomic_bool m_exiting{false};
    std::thread m_vsync_thread;
    VkFence vsync_fence = VK_NULL_HANDLE;
    layer::device_private_data& m_device_data;
};

} // namespace wsi
