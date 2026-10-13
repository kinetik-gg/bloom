#include "gpu_operation_batch_private.hpp"

namespace bloom::render::operation_batch_detail {
bool State::record() {
    const auto* api = control->device.getDispatcher();
    const VkDevice device = static_cast<VkDevice>(*control->device);
    if (api->vkResetCommandBuffer(command, 0) != VK_SUCCESS ||
        api->vkResetDescriptorPool(device, descriptors, 0) != VK_SUCCESS ||
        api->vkResetFences(device, 1, &fence) != VK_SUCCESS)
        return false;
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    if (api->vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
        return false;
    for (std::uint32_t index = 0; index < count; ++index) {
        auto& job = jobs[index];
        const auto& pipeline = pipelines[job.kind];
        const VkDescriptorSetLayout layout =
            static_cast<VkDescriptorSetLayout>(*pipeline.descriptorSetLayout);
        const VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                     nullptr, descriptors, 1, &layout};
        if (api->vkAllocateDescriptorSets(device, &allocation, &job.descriptors) != VK_SUCCESS)
            return false;
        std::array<VkDescriptorImageInfo, 3> images{};
        std::array<VkDescriptorBufferInfo, 3> buffers{};
        std::array<VkWriteDescriptorSet, 5> writes{};
        const auto image = [&images, &writes, &job](const std::uint32_t binding,
                                                    const GpuImageImpl* value) {
            images[binding] = {VK_NULL_HANDLE, value->view, VK_IMAGE_LAYOUT_GENERAL};
            writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               nullptr,
                               job.descriptors,
                               binding,
                               0,
                               1,
                               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               &images[binding],
                               nullptr,
                               nullptr};
        };
        const auto buffer = [&buffers, &writes, &job](const std::uint32_t binding,
                                                      const std::uint32_t slot) {
            buffers[slot] = {job.metadata[slot].buffer, 0, job.metadata[slot].bytes};
            writes[binding] = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, job.descriptors, binding, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,      nullptr, &buffers[slot],  nullptr};
        };
        std::uint32_t bindingCount = 1;
        if (job.kind == 0) {
            image(0, job.image.get());
        } else {
            image(0, job.source);
            image(1, job.kind == 3 ? job.destination : job.image.get());
            if (job.kind == 3) {
                image(2, job.image.get());
                buffer(3, 2);
                bindingCount = 4;
            } else if (job.kind == 1) {
                buffer(2, 0);
                buffer(3, 1);
                buffer(4, 2);
                bindingCount = 5;
            } else {
                buffer(2, 0);
                buffer(3, 2);
                bindingCount = 4;
            }
        }
        api->vkUpdateDescriptorSets(device, bindingCount, writes.data(), 0, nullptr);
        for (const auto& metadata : job.metadata)
            metadata.recordInitialization(command);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = job.image->image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        api->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                  1, &barrier);
        const VkPipeline nativePipeline = static_cast<VkPipeline>(*pipeline.pipeline);
        const VkPipelineLayout nativeLayout =
            static_cast<VkPipelineLayout>(*pipeline.pipelineLayout);
        api->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, nativePipeline);
        api->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, nativeLayout, 0, 1,
                                     &job.descriptors, 0, nullptr);
        api->vkCmdPushConstants(command, nativeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                job.pushBytes, job.push.data());
        api->vkCmdDispatch(
            command, static_cast<std::uint32_t>(compositeDispatchGroupCount(job.image->width)),
            job.image->height, 1);
        // Every produced image has an explicit write->read dependency before its first consumer.
        // This also leaves the final resident output in the same GENERAL/readable state as the
        // existing individual kernels, including consumers submitted later on this queue.
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        api->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                  1, &barrier);
        if (job.kind != 0)
            composite_detail::statusToHost(*control, command, job.metadata[2].buffer,
                                           sizeof(std::uint32_t));
    }
    if (api->vkEndCommandBuffer(command) != VK_SUCCESS)
        return false;
    return true;
}

} // namespace bloom::render::operation_batch_detail
