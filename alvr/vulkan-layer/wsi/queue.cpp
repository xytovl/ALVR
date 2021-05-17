#include "queue.hpp"

#include "layer/private_data.hpp"

wsi::queue::queue(layer::device_private_data& device_data, uint32_t queue_family_index, uint32_t queue_index):
	m_device_data(device_data)
{
  m_device_data.disp.GetDeviceQueue(m_device_data.device, queue_family_index, queue_index, &m_queue);
  m_device_data.SetDeviceLoaderData(m_device_data.device, m_queue);
}

void wsi::queue::signal(VkSemaphore semaphore, VkFence fence)
{
	std::unique_lock<std::mutex> lock(m_mutex);
  if (VK_NULL_HANDLE != semaphore || VK_NULL_HANDLE != fence) {
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    if (VK_NULL_HANDLE != semaphore) {
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &semaphore;
    }

    submit.commandBufferCount = 0;
    submit.pCommandBuffers = nullptr;
    auto retval = m_device_data.disp.QueueSubmit(m_queue, 1, &submit, fence);
    assert(retval == VK_SUCCESS);
  }
}

void wsi::queue::waitIdle()
{
  m_device_data.disp.QueueWaitIdle(m_queue);
}
