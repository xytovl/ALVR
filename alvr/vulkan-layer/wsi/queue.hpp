#pragma once

#include <cstdint>
#include <mutex>
#include <vulkan/vulkan.h>

namespace layer
{
class device_private_data;
}

namespace wsi {

class queue
{
public:
	queue(layer::device_private_data& device_data, uint32_t queue_family_index, uint32_t queue_index);
	void signal(VkSemaphore semaphore, VkFence fence);
	void waitIdle();
private:
	layer::device_private_data& m_device_data;
	VkQueue m_queue;
	std::mutex m_mutex;
};

}
