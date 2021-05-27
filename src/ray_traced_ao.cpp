#include "ray_traced_ao.h"
#include "common_resources.h"
#include "g_buffer.h"
#include "utilities.h"
#include <profiler.h>
#include <macros.h>
#include <imgui.h>

#define MAX_RAY_TRACE_IMAGE_MIP_MAPS 5

// -----------------------------------------------------------------------------------------------------------------------------------

struct RayTracePushConstants
{
    uint32_t num_rays;
    uint32_t num_frames;
    float    ray_length;
    float    power;
    float    bias;
    uint32_t sampler_type;
};

// -----------------------------------------------------------------------------------------------------------------------------------

struct TemporalReprojectionPushConstants
{
    float alpha;
};

// -----------------------------------------------------------------------------------------------------------------------------------

struct GaussianBlurPushConstants
{
    glm::vec4  z_buffer_params;
    glm::ivec2 direction;
    int32_t    radius;
};

// -----------------------------------------------------------------------------------------------------------------------------------

struct RecurrentBlurPushConstants
{
    glm::vec4 z_buffer_params;
    float     radius;
    uint32_t  num_frames;
    uint32_t  self_stabilize;
};

// -----------------------------------------------------------------------------------------------------------------------------------

struct UpsamplePushConstants
{
    glm::vec4 z_buffer_params;
};

// -----------------------------------------------------------------------------------------------------------------------------------

RayTracedAO::RayTracedAO(std::weak_ptr<dw::vk::Backend> backend, CommonResources* common_resources, GBuffer* g_buffer) :
    m_backend(backend), m_common_resources(common_resources), m_g_buffer(g_buffer)
{
    auto vk_backend = m_backend.lock();
    m_width         = vk_backend->swap_chain_extents().width / 2;
    m_height        = vk_backend->swap_chain_extents().height / 2;

    create_images();
    create_descriptor_sets();
    write_descriptor_sets();
    create_pipeline();
}

// -----------------------------------------------------------------------------------------------------------------------------------

RayTracedAO::~RayTracedAO()
{
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::render(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    if (m_enabled)
    {
        DW_SCOPED_SAMPLE("Ambient Occlusion", cmd_buf);

        clear_images(cmd_buf);
        ray_trace(cmd_buf);

        if (m_denoise)
        {
            denoise(cmd_buf);
            upsample(cmd_buf);
        }
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::gui()
{
    ImGui::PushID("RTAO");
    ImGui::Checkbox("Enabled", &m_enabled);
    ImGui::Checkbox("Denoise", &m_denoise);
    ImGui::Checkbox("Recurrent Blur", &m_use_recurrent_blur);

    if (m_use_recurrent_blur)
    {
        ImGui::Checkbox("Self-Stabilize", &m_recurrent_blur.self_stabilize);
        ImGui::Checkbox("Feedback", &m_recurrent_blur.feedback);
    }

    ImGui::SliderInt("Num Rays", &m_ray_trace.num_rays, 1, 8);
    ImGui::SliderFloat("Ray Length", &m_ray_trace.ray_length, 1.0f, 100.0f);
    ImGui::SliderFloat("Power", &m_ray_trace.power, 1.0f, 5.0f);
    ImGui::InputFloat("Bias", &m_ray_trace.bias);
    ImGui::SliderFloat("Temporal Alpha", &m_temporal_reprojection.alpha, 0.0f, 0.5f);

    if (m_use_recurrent_blur)
        ImGui::SliderInt("Blur Radius", &m_recurrent_blur.blur_radius, 1, 30);
    else
        ImGui::SliderInt("Blur Radius", &m_gaussian_blur.blur_radius, 1, 20);

    ImGui::PopID();
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::create_images()
{
    auto backend = m_backend.lock();

    // Ray Trace
    {
        m_ray_trace.image = dw::vk::Image::create(backend, VK_IMAGE_TYPE_2D, m_width, m_height, 1, MAX_RAY_TRACE_IMAGE_MIP_MAPS, 1, VK_FORMAT_R8_UNORM, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT);
        m_ray_trace.image->set_name("AO Ray Trace");

        m_ray_trace.view = dw::vk::ImageView::create(backend, m_ray_trace.image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
        m_ray_trace.view->set_name("AO Ray Trace");

        m_ray_trace.all_mips_view = dw::vk::ImageView::create(backend, m_ray_trace.image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, MAX_RAY_TRACE_IMAGE_MIP_MAPS);
        m_ray_trace.all_mips_view->set_name("AO Max Mips Ray Trace");
    }

    // Temporal Reprojection
    {
        for (int i = 0; i < 2; i++)
        {
            m_temporal_reprojection.color_image[i] = dw::vk::Image::create(backend, VK_IMAGE_TYPE_2D, m_width, m_height, 1, 5, 1, VK_FORMAT_R16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
            m_temporal_reprojection.color_image[i]->set_name("AO Denoise Reprojection " + std::to_string(i));

            m_temporal_reprojection.color_view[i] = dw::vk::ImageView::create(backend, m_temporal_reprojection.color_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
            m_temporal_reprojection.color_view[i]->set_name("AO Denoise Reprojection " + std::to_string(i));

            m_temporal_reprojection.history_length_image[i] = dw::vk::Image::create(backend, VK_IMAGE_TYPE_2D, m_width, m_height, 1, 1, 1, VK_FORMAT_R16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
            m_temporal_reprojection.history_length_image[i]->set_name("AO Denoise Reprojection History " + std::to_string(i));

            m_temporal_reprojection.history_length_view[i] = dw::vk::ImageView::create(backend, m_temporal_reprojection.history_length_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
            m_temporal_reprojection.history_length_view[i]->set_name("AO Denoise Reprojection History " + std::to_string(i));
        }
    }

    // Downsample
    {
        for (int i = 0; i < 2; i++)
        {
            m_downsample.image_view_mip1[i] = dw::vk::ImageView::create(backend, m_temporal_reprojection.color_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 1);
            m_downsample.image_view_mip2[i] = dw::vk::ImageView::create(backend, m_temporal_reprojection.color_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 2);
            m_downsample.image_view_mip3[i] = dw::vk::ImageView::create(backend, m_temporal_reprojection.color_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 3);
            m_downsample.image_view_mip4[i] = dw::vk::ImageView::create(backend, m_temporal_reprojection.color_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 4);
        }
    }

    // Gaussian Blur
    {
        for (int i = 0; i < 2; i++)
        {
            m_gaussian_blur.image[i] = dw::vk::Image::create(backend, VK_IMAGE_TYPE_2D, m_width, m_height, 1, 1, 1, VK_FORMAT_R16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
            m_gaussian_blur.image[i]->set_name("AO Denoise Blur " + std::to_string(i));

            m_gaussian_blur.image_view[i] = dw::vk::ImageView::create(backend, m_gaussian_blur.image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
            m_gaussian_blur.image_view[i]->set_name("AO Denoise Blur " + std::to_string(i));
        }
    }

    // Recurrent Blur
    {
        m_recurrent_blur.image = dw::vk::Image::create(backend, VK_IMAGE_TYPE_2D, m_width, m_height, 1, 1, 1, VK_FORMAT_R16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
        m_recurrent_blur.image->set_name("AO Denoise Recurrent Blur");

        m_recurrent_blur.image_view = dw::vk::ImageView::create(backend, m_recurrent_blur.image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
        m_recurrent_blur.image_view->set_name("AO Denoise Recurrent Blur");
    }

    // Upsample
    {
        m_upsample.image = dw::vk::Image::create(backend, VK_IMAGE_TYPE_2D, m_width * 2, m_height * 2, 1, 1, 1, VK_FORMAT_R16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
        m_upsample.image->set_name("AO Upsample");

        m_upsample.image_view = dw::vk::ImageView::create(backend, m_upsample.image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
        m_upsample.image_view->set_name("AO Upsample");
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::create_descriptor_sets()
{
    auto backend = m_backend.lock();

    // Ray Trace
    {
        m_ray_trace.write_ds = backend->allocate_descriptor_set(m_common_resources->storage_image_ds_layout);
        m_ray_trace.write_ds->set_name("AO Ray Trace Write");

        m_ray_trace.read_ds = backend->allocate_descriptor_set(m_common_resources->combined_sampler_ds_layout);
        m_ray_trace.read_ds->set_name("AO Ray Trace Read");
    }

    // Temporal Reprojection
    {
        {
            dw::vk::DescriptorSetLayout::Desc desc;

            desc.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
            desc.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);

            m_temporal_reprojection.write_ds_layout = dw::vk::DescriptorSetLayout::create(backend, desc);
            m_temporal_reprojection.write_ds_layout->set_name("AO Reprojection Write DS Layout");
        }

        {
            dw::vk::DescriptorSetLayout::Desc desc;

            desc.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
            desc.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);

            m_temporal_reprojection.read_ds_layout = dw::vk::DescriptorSetLayout::create(backend, desc);
            m_temporal_reprojection.read_ds_layout->set_name("AO Reprojection Read DS Layout");
        }

        for (int i = 0; i < 2; i++)
        {
            m_temporal_reprojection.write_ds[i] = backend->allocate_descriptor_set(m_temporal_reprojection.write_ds_layout);
            m_temporal_reprojection.write_ds[i]->set_name("AO Reprojection Write " + std::to_string(i));

            m_temporal_reprojection.read_ds[i] = backend->allocate_descriptor_set(m_temporal_reprojection.read_ds_layout);
            m_temporal_reprojection.read_ds[i]->set_name("AO Reprojection Read " + std::to_string(i));

            m_temporal_reprojection.output_read_ds[i] = backend->allocate_descriptor_set(m_common_resources->combined_sampler_ds_layout);
            m_temporal_reprojection.output_read_ds[i]->set_name("AO Reprojection Output Read " + std::to_string(i));

            m_temporal_reprojection.output_bilinear_read_ds[i] = backend->allocate_descriptor_set(m_common_resources->combined_sampler_ds_layout);
            m_temporal_reprojection.output_bilinear_read_ds[i]->set_name("AO Reprojection Bilinear Output Read " + std::to_string(i));
        }
    }

    // Downsample
    {
        {
            dw::vk::DescriptorSetLayout::Desc desc;

            desc.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
            desc.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
            desc.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
            desc.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);

            m_downsample.write_ds_layout = dw::vk::DescriptorSetLayout::create(backend, desc);
            m_downsample.write_ds_layout->set_name("Downsample Write DS Layout");
        }

        for (int i = 0; i < 2; i++)
        {
            m_downsample.write_ds[i] = backend->allocate_descriptor_set(m_downsample.write_ds_layout);
            m_downsample.write_ds[i]->set_name("Downsample Write " + std::to_string(i));
        }
    }

    // Gaussian Blur
    {
        for (int i = 0; i < 2; i++)
        {
            m_gaussian_blur.write_ds[i] = backend->allocate_descriptor_set(m_common_resources->storage_image_ds_layout);
            m_gaussian_blur.write_ds[i]->set_name("AO Blur Write " + std::to_string(i));

            m_gaussian_blur.read_ds[i] = backend->allocate_descriptor_set(m_common_resources->combined_sampler_ds_layout);
            m_gaussian_blur.read_ds[i]->set_name("AO Blur Read " + std::to_string(i));
        }
    }

    // Recurrent Blur
    {
        m_recurrent_blur.write_ds = backend->allocate_descriptor_set(m_common_resources->storage_image_ds_layout);
        m_recurrent_blur.write_ds->set_name("AO Recurrent Blur Write");

        m_recurrent_blur.read_ds = backend->allocate_descriptor_set(m_common_resources->combined_sampler_ds_layout);
        m_recurrent_blur.read_ds->set_name("AO Recurrent Blur Read");
    }

    // Upsample
    {
        m_upsample.write_ds = backend->allocate_descriptor_set(m_common_resources->storage_image_ds_layout);
        m_upsample.write_ds->set_name("AO Upsample Write");

        m_upsample.read_ds = backend->allocate_descriptor_set(m_common_resources->combined_sampler_ds_layout);
        m_upsample.read_ds->set_name("AO Upsample Read");
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::write_descriptor_sets()
{
    auto backend = m_backend.lock();

    // Ray Trace
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(1);
        write_datas.reserve(1);

        VkDescriptorImageInfo storage_image_info;

        storage_image_info.sampler     = VK_NULL_HANDLE;
        storage_image_info.imageView   = m_ray_trace.view->handle();
        storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        image_infos.push_back(storage_image_info);

        DW_ZERO_MEMORY(write_data);

        write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_data.descriptorCount = 1;
        write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write_data.pImageInfo      = &image_infos.back();
        write_data.dstBinding      = 0;
        write_data.dstSet          = m_ray_trace.write_ds->handle();

        write_datas.push_back(write_data);

        vkUpdateDescriptorSets(backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(1);
        write_datas.reserve(1);

        VkDescriptorImageInfo sampler_image_info;

        sampler_image_info.sampler     = backend->nearest_sampler()->handle();
        sampler_image_info.imageView   = m_ray_trace.view->handle();
        sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        image_infos.push_back(sampler_image_info);

        DW_ZERO_MEMORY(write_data);

        write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_data.descriptorCount = 1;
        write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write_data.pImageInfo      = &image_infos.back();
        write_data.dstBinding      = 0;
        write_data.dstSet          = m_ray_trace.read_ds->handle();

        write_datas.push_back(write_data);

        vkUpdateDescriptorSets(backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    // Temporal Reprojection
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(4);
        write_datas.reserve(4);

        for (int i = 0; i < 2; i++)
        {
            {
                VkDescriptorImageInfo storage_image_info;

                storage_image_info.sampler     = VK_NULL_HANDLE;
                storage_image_info.imageView   = m_temporal_reprojection.color_view[i]->handle();
                storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                image_infos.push_back(storage_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 0;
                write_data.dstSet          = m_temporal_reprojection.write_ds[i]->handle();

                write_datas.push_back(write_data);
            }

            {
                VkDescriptorImageInfo storage_image_info;

                storage_image_info.sampler     = VK_NULL_HANDLE;
                storage_image_info.imageView   = m_temporal_reprojection.history_length_view[i]->handle();
                storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                image_infos.push_back(storage_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 1;
                write_data.dstSet          = m_temporal_reprojection.write_ds[i]->handle();

                write_datas.push_back(write_data);
            }
        }

        vkUpdateDescriptorSets(backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(4);
        write_datas.reserve(4);

        for (int i = 0; i < 2; i++)
        {
            {
                VkDescriptorImageInfo sampler_image_info;

                sampler_image_info.sampler     = backend->nearest_sampler()->handle();
                sampler_image_info.imageView   = m_temporal_reprojection.color_view[i]->handle();
                sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                image_infos.push_back(sampler_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 0;
                write_data.dstSet          = m_temporal_reprojection.read_ds[i]->handle();

                write_datas.push_back(write_data);
            }

            {
                VkDescriptorImageInfo sampler_image_info;

                sampler_image_info.sampler     = backend->nearest_sampler()->handle();
                sampler_image_info.imageView   = m_temporal_reprojection.history_length_view[i]->handle();
                sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                image_infos.push_back(sampler_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 1;
                write_data.dstSet          = m_temporal_reprojection.read_ds[i]->handle();

                write_datas.push_back(write_data);
            }
        }

        vkUpdateDescriptorSets(backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(2);
        write_datas.reserve(2);

        for (int i = 0; i < 2; i++)
        {
            {
                VkDescriptorImageInfo sampler_image_info;

                sampler_image_info.sampler     = backend->nearest_sampler()->handle();
                sampler_image_info.imageView   = m_temporal_reprojection.color_view[i]->handle();
                sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                image_infos.push_back(sampler_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 0;
                write_data.dstSet          = m_temporal_reprojection.output_read_ds[i]->handle();

                write_datas.push_back(write_data);
            }
        }

        vkUpdateDescriptorSets(backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(2);
        write_datas.reserve(2);

        for (int i = 0; i < 2; i++)
        {
            {
                VkDescriptorImageInfo sampler_image_info;

                sampler_image_info.sampler     = backend->bilinear_sampler()->handle();
                sampler_image_info.imageView   = m_temporal_reprojection.color_view[i]->handle();
                sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                image_infos.push_back(sampler_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 0;
                write_data.dstSet          = m_temporal_reprojection.output_bilinear_read_ds[i]->handle();

                write_datas.push_back(write_data);
            }
        }

        vkUpdateDescriptorSets(backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    // Downsample
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(8);
        write_datas.reserve(8);

        for (int i = 0; i < 2; i++)
        {
            {
                VkDescriptorImageInfo storage_image_info;

                storage_image_info.sampler     = VK_NULL_HANDLE;
                storage_image_info.imageView   = m_downsample.image_view_mip1[i]->handle();
                storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                image_infos.push_back(storage_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 0;
                write_data.dstSet          = m_downsample.write_ds[i]->handle();

                write_datas.push_back(write_data);
            }

            {
                VkDescriptorImageInfo storage_image_info;

                storage_image_info.sampler     = VK_NULL_HANDLE;
                storage_image_info.imageView   = m_downsample.image_view_mip2[i]->handle();
                storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                image_infos.push_back(storage_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 1;
                write_data.dstSet          = m_downsample.write_ds[i]->handle();

                write_datas.push_back(write_data);
            }

            {
                VkDescriptorImageInfo storage_image_info;

                storage_image_info.sampler     = VK_NULL_HANDLE;
                storage_image_info.imageView   = m_downsample.image_view_mip3[i]->handle();
                storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                image_infos.push_back(storage_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 2;
                write_data.dstSet          = m_downsample.write_ds[i]->handle();

                write_datas.push_back(write_data);
            }

            {
                VkDescriptorImageInfo storage_image_info;

                storage_image_info.sampler     = VK_NULL_HANDLE;
                storage_image_info.imageView   = m_downsample.image_view_mip4[i]->handle();
                storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                image_infos.push_back(storage_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 3;
                write_data.dstSet          = m_downsample.write_ds[i]->handle();

                write_datas.push_back(write_data);
            }
        }

        vkUpdateDescriptorSets(backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    // Gaussian Blur
    {
        for (int i = 0; i < 2; i++)
        {
            // write
            {
                VkDescriptorImageInfo storage_image_info;

                storage_image_info.sampler     = VK_NULL_HANDLE;
                storage_image_info.imageView   = m_gaussian_blur.image_view[i]->handle();
                storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                VkWriteDescriptorSet write_data;

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write_data.pImageInfo      = &storage_image_info;
                write_data.dstBinding      = 0;
                write_data.dstSet          = m_gaussian_blur.write_ds[i]->handle();

                vkUpdateDescriptorSets(backend->device(), 1, &write_data, 0, nullptr);
            }

            // read
            {
                VkDescriptorImageInfo sampler_image_info;

                sampler_image_info.sampler     = backend->nearest_sampler()->handle();
                sampler_image_info.imageView   = m_gaussian_blur.image_view[i]->handle();
                sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet write_data;

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write_data.pImageInfo      = &sampler_image_info;
                write_data.dstBinding      = 0;
                write_data.dstSet          = m_gaussian_blur.read_ds[i]->handle();

                vkUpdateDescriptorSets(backend->device(), 1, &write_data, 0, nullptr);
            }
        }
    }

    // Recurrent Blur
    {
        VkDescriptorImageInfo storage_image_info;

        storage_image_info.sampler     = VK_NULL_HANDLE;
        storage_image_info.imageView   = m_recurrent_blur.image_view->handle();
        storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write_data;

        DW_ZERO_MEMORY(write_data);

        write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_data.descriptorCount = 1;
        write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write_data.pImageInfo      = &storage_image_info;
        write_data.dstBinding      = 0;
        write_data.dstSet          = m_recurrent_blur.write_ds->handle();

        vkUpdateDescriptorSets(backend->device(), 1, &write_data, 0, nullptr);
    }

    {
        VkDescriptorImageInfo sampler_image_info;

        sampler_image_info.sampler     = backend->nearest_sampler()->handle();
        sampler_image_info.imageView   = m_recurrent_blur.image_view->handle();
        sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write_data;

        DW_ZERO_MEMORY(write_data);

        write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_data.descriptorCount = 1;
        write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write_data.pImageInfo      = &sampler_image_info;
        write_data.dstBinding      = 0;
        write_data.dstSet          = m_recurrent_blur.read_ds->handle();

        vkUpdateDescriptorSets(backend->device(), 1, &write_data, 0, nullptr);
    }

    // Upsample
    {
        // write
        {
            VkDescriptorImageInfo storage_image_info;

            storage_image_info.sampler     = VK_NULL_HANDLE;
            storage_image_info.imageView   = m_upsample.image_view->handle();
            storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet write_data;

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write_data.pImageInfo      = &storage_image_info;
            write_data.dstBinding      = 0;
            write_data.dstSet          = m_upsample.write_ds->handle();

            vkUpdateDescriptorSets(backend->device(), 1, &write_data, 0, nullptr);
        }

        // read
        {
            VkDescriptorImageInfo sampler_image_info;

            sampler_image_info.sampler     = backend->nearest_sampler()->handle();
            sampler_image_info.imageView   = m_upsample.image_view->handle();
            sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet write_data;

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write_data.pImageInfo      = &sampler_image_info;
            write_data.dstBinding      = 0;
            write_data.dstSet          = m_upsample.read_ds->handle();

            vkUpdateDescriptorSets(backend->device(), 1, &write_data, 0, nullptr);
        }
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::create_pipeline()
{
    auto backend = m_backend.lock();

    // Ray Trace
    {
        dw::vk::ShaderModule::Ptr shader_module = dw::vk::ShaderModule::create_from_file(backend, "shaders/ao_ray_trace.comp.spv");

        dw::vk::PipelineLayout::Desc pl_desc;

        pl_desc.add_descriptor_set_layout(m_common_resources->pillars_scene->descriptor_set_layout());
        pl_desc.add_descriptor_set_layout(m_common_resources->storage_image_ds_layout);
        pl_desc.add_descriptor_set_layout(m_common_resources->per_frame_ds_layout);
        pl_desc.add_descriptor_set_layout(m_g_buffer->ds_layout());
        pl_desc.add_descriptor_set_layout(m_common_resources->blue_noise_ds_layout);

        pl_desc.add_push_constant_range(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RayTracePushConstants));

        m_ray_trace.pipeline_layout = dw::vk::PipelineLayout::create(backend, pl_desc);
        m_ray_trace.pipeline_layout->set_name("AO Ray Trace Pipeline Layout");

        dw::vk::ComputePipeline::Desc desc;

        desc.set_shader_stage(shader_module, "main");
        desc.set_pipeline_layout(m_ray_trace.pipeline_layout);

        m_ray_trace.pipeline = dw::vk::ComputePipeline::create(backend, desc);
    }

    // Temporal Reprojection
    {
        dw::vk::PipelineLayout::Desc desc;

        desc.add_descriptor_set_layout(m_temporal_reprojection.write_ds_layout);
        desc.add_descriptor_set_layout(m_g_buffer->ds_layout());
        desc.add_descriptor_set_layout(m_g_buffer->ds_layout());
        desc.add_descriptor_set_layout(m_common_resources->combined_sampler_ds_layout);
        desc.add_descriptor_set_layout(m_common_resources->combined_sampler_ds_layout);
        desc.add_descriptor_set_layout(m_temporal_reprojection.read_ds_layout);

        desc.add_push_constant_range(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TemporalReprojectionPushConstants));

        m_temporal_reprojection.pipeline_layout = dw::vk::PipelineLayout::create(backend, desc);
        m_temporal_reprojection.pipeline_layout->set_name("AO Reprojection Pipeline Layout");

        dw::vk::ShaderModule::Ptr module = dw::vk::ShaderModule::create_from_file(backend, "shaders/ao_denoise_reprojection.comp.spv");

        dw::vk::ComputePipeline::Desc comp_desc;

        comp_desc.set_pipeline_layout(m_temporal_reprojection.pipeline_layout);
        comp_desc.set_shader_stage(module, "main");

        m_temporal_reprojection.pipeline = dw::vk::ComputePipeline::create(backend, comp_desc);
    }

    // Downsample
    {
        dw::vk::PipelineLayout::Desc desc;

        desc.add_descriptor_set_layout(m_downsample.write_ds_layout);
        desc.add_descriptor_set_layout(m_common_resources->combined_sampler_ds_layout);

        m_downsample.pipeline_layout = dw::vk::PipelineLayout::create(backend, desc);
        m_downsample.pipeline_layout->set_name("Downsample Pipeline Layout");

        dw::vk::ShaderModule::Ptr module = dw::vk::ShaderModule::create_from_file(backend, "shaders/ao_denoise_downsample.comp.spv");

        dw::vk::ComputePipeline::Desc comp_desc;

        comp_desc.set_pipeline_layout(m_downsample.pipeline_layout);
        comp_desc.set_shader_stage(module, "main");

        m_downsample.pipeline = dw::vk::ComputePipeline::create(backend, comp_desc);
    }

    // Gaussian Blur
    {
        dw::vk::PipelineLayout::Desc desc;

        desc.add_descriptor_set_layout(m_common_resources->storage_image_ds_layout);
        desc.add_descriptor_set_layout(m_common_resources->combined_sampler_ds_layout);
        desc.add_descriptor_set_layout(m_temporal_reprojection.read_ds_layout);
        desc.add_descriptor_set_layout(m_g_buffer->ds_layout());

        desc.add_push_constant_range(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GaussianBlurPushConstants));

        m_gaussian_blur.layout = dw::vk::PipelineLayout::create(backend, desc);
        m_gaussian_blur.layout->set_name("AO Blur Pipeline Layout");

        dw::vk::ShaderModule::Ptr module = dw::vk::ShaderModule::create_from_file(backend, "shaders/ao_denoise_gaussian_blur.comp.spv");

        dw::vk::ComputePipeline::Desc comp_desc;

        comp_desc.set_pipeline_layout(m_gaussian_blur.layout);
        comp_desc.set_shader_stage(module, "main");

        m_gaussian_blur.pipeline = dw::vk::ComputePipeline::create(backend, comp_desc);
    }

    // Recurrent Blur
    {
        dw::vk::PipelineLayout::Desc desc;

        desc.add_descriptor_set_layout(m_common_resources->storage_image_ds_layout);
        desc.add_descriptor_set_layout(m_common_resources->combined_sampler_ds_layout);
        desc.add_descriptor_set_layout(m_temporal_reprojection.read_ds_layout);
        desc.add_descriptor_set_layout(m_g_buffer->ds_layout());

        desc.add_push_constant_range(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RecurrentBlurPushConstants));

        m_recurrent_blur.layout = dw::vk::PipelineLayout::create(backend, desc);
        m_recurrent_blur.layout->set_name("AO Recurrent Blur Pipeline Layout");

        dw::vk::ShaderModule::Ptr module = dw::vk::ShaderModule::create_from_file(backend, "shaders/ao_denoise_recurrent_blur.comp.spv");

        dw::vk::ComputePipeline::Desc comp_desc;

        comp_desc.set_pipeline_layout(m_recurrent_blur.layout);
        comp_desc.set_shader_stage(module, "main");

        m_recurrent_blur.pipeline = dw::vk::ComputePipeline::create(backend, comp_desc);
    }

    // Upsample
    {
        dw::vk::PipelineLayout::Desc desc;

        desc.add_descriptor_set_layout(m_common_resources->storage_image_ds_layout);
        desc.add_descriptor_set_layout(m_common_resources->combined_sampler_ds_layout);
        desc.add_descriptor_set_layout(m_g_buffer->ds_layout());

        desc.add_push_constant_range(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpsamplePushConstants));

        m_upsample.layout = dw::vk::PipelineLayout::create(backend, desc);
        m_upsample.layout->set_name("AO Upsample Pipeline Layout");

        dw::vk::ShaderModule::Ptr module = dw::vk::ShaderModule::create_from_file(backend, "shaders/ao_upsample.comp.spv");

        dw::vk::ComputePipeline::Desc comp_desc;

        comp_desc.set_pipeline_layout(m_upsample.layout);
        comp_desc.set_shader_stage(module, "main");

        m_upsample.pipeline = dw::vk::ComputePipeline::create(backend, comp_desc);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::clear_images(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    if (m_common_resources->first_frame)
    {
        VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkClearColorValue color;

        color.float32[0] = 0.0f;
        color.float32[1] = 0.0f;
        color.float32[2] = 0.0f;
        color.float32[3] = 0.0f;

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_temporal_reprojection.history_length_image[!m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_temporal_reprojection.color_image[!m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            subresource_range);

        vkCmdClearColorImage(cmd_buf->handle(), m_temporal_reprojection.history_length_image[!m_common_resources->ping_pong]->handle(), VK_IMAGE_LAYOUT_GENERAL, &color, 1, &subresource_range);
        vkCmdClearColorImage(cmd_buf->handle(), m_temporal_reprojection.color_image[!m_common_resources->ping_pong]->handle(), VK_IMAGE_LAYOUT_GENERAL, &color, 1, &subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_temporal_reprojection.history_length_image[!m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_temporal_reprojection.color_image[!m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_recurrent_blur.image->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::ray_trace(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE("Ray Trace", cmd_buf);

    auto backend = m_backend.lock();

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    std::vector<VkMemoryBarrier> memory_barriers = {
        memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
    };

    std::vector<VkImageMemoryBarrier> image_barriers = {
        image_memory_barrier(m_ray_trace.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
    };

    pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_ray_trace.pipeline->handle());

    RayTracePushConstants push_constants;

    push_constants.num_frames   = m_common_resources->num_frames;
    push_constants.num_rays     = m_ray_trace.num_rays;
    push_constants.ray_length   = m_ray_trace.ray_length;
    push_constants.power        = m_ray_trace.power;
    push_constants.bias         = m_ray_trace.bias;
    push_constants.sampler_type = m_common_resources->sampler_type;

    vkCmdPushConstants(cmd_buf->handle(), m_ray_trace.pipeline_layout->handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

    const uint32_t dynamic_offset = m_common_resources->ubo_size * backend->current_frame_idx();

    VkDescriptorSet descriptor_sets[] = {
        m_common_resources->current_scene->descriptor_set()->handle(),
        m_ray_trace.write_ds->handle(),
        m_common_resources->per_frame_ds->handle(),
        m_g_buffer->output_ds()->handle(),
        m_common_resources->blue_noise_ds[BLUE_NOISE_2SPP]->handle()
    };

    vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_ray_trace.pipeline_layout->handle(), 0, 5, descriptor_sets, 1, &dynamic_offset);

    const int NUM_THREADS_X = 32;
    const int NUM_THREADS_Y = 32;

    vkCmdDispatch(cmd_buf->handle(), static_cast<uint32_t>(ceil(float(m_width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(NUM_THREADS_Y))), 1);

    m_ray_trace.image->generate_mipmaps(cmd_buf, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, VK_FILTER_LINEAR);
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::denoise(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE("Denoise", cmd_buf);

    temporal_reprojection(cmd_buf);
    downsample(cmd_buf);

    if (m_use_recurrent_blur)
        recurrent_blur(cmd_buf);
    else
        gaussian_blur(cmd_buf);
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::upsample(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE("Upsample", cmd_buf);

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_upsample.image->handle(),
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,
        subresource_range);

    vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_upsample.pipeline->handle());

    UpsamplePushConstants push_constants;

    push_constants.z_buffer_params = m_common_resources->z_buffer_params;

    vkCmdPushConstants(cmd_buf->handle(), m_upsample.layout->handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

    VkDescriptorSet descriptor_sets[] = {
        m_upsample.write_ds->handle(),
        m_use_recurrent_blur ? m_recurrent_blur.read_ds->handle() : m_gaussian_blur.read_ds[1]->handle(),
        m_g_buffer->output_ds()->handle()
    };

    vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_upsample.layout->handle(), 0, 3, descriptor_sets, 0, nullptr);

    const int NUM_THREADS_X = 32;
    const int NUM_THREADS_Y = 32;

    vkCmdDispatch(cmd_buf->handle(), static_cast<uint32_t>(ceil(float(m_upsample.image->width()) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_upsample.image->height()) / float(NUM_THREADS_Y))), 1);

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_upsample.image->handle(),
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        subresource_range);
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::temporal_reprojection(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE("Temporal Reprojection", cmd_buf);

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    {
        std::vector<VkMemoryBarrier> memory_barriers = {
            memory_barrier(VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
        };

        std::vector<VkImageMemoryBarrier> image_barriers = {
            image_memory_barrier(m_temporal_reprojection.color_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT),
            image_memory_barrier(m_temporal_reprojection.history_length_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
        };

        pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_temporal_reprojection.pipeline->handle());

    TemporalReprojectionPushConstants push_constants;

    push_constants.alpha = m_temporal_reprojection.alpha;

    vkCmdPushConstants(cmd_buf->handle(), m_temporal_reprojection.pipeline_layout->handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

    VkDescriptorSet descriptor_sets[] = {
        m_temporal_reprojection.write_ds[m_common_resources->ping_pong]->handle(),
        m_g_buffer->output_ds()->handle(),
        m_g_buffer->history_ds()->handle(),
        m_ray_trace.read_ds->handle(),
        (m_use_recurrent_blur && m_recurrent_blur.feedback) ? m_recurrent_blur.read_ds->handle() : m_temporal_reprojection.output_read_ds[!m_common_resources->ping_pong]->handle(),
        m_temporal_reprojection.read_ds[!m_common_resources->ping_pong]->handle()
    };

    vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_temporal_reprojection.pipeline_layout->handle(), 0, 6, descriptor_sets, 0, nullptr);

    const int NUM_THREADS_X = 32;
    const int NUM_THREADS_Y = 32;

    vkCmdDispatch(cmd_buf->handle(), static_cast<uint32_t>(ceil(float(m_width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(NUM_THREADS_Y))), 1);

    {
        std::vector<VkMemoryBarrier> memory_barriers = {
            memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
        };

        std::vector<VkImageMemoryBarrier> image_barriers = {
            image_memory_barrier(m_temporal_reprojection.color_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, subresource_range, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            image_memory_barrier(m_temporal_reprojection.history_length_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, subresource_range, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
        };

        pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::downsample(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE("Downsample", cmd_buf);

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 4, 0, 1 };

    {
        std::vector<VkMemoryBarrier> memory_barriers = {
            memory_barrier(VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
        };

        std::vector<VkImageMemoryBarrier> image_barriers = {
            image_memory_barrier(m_temporal_reprojection.color_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
        };

        pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_downsample.pipeline->handle());

    VkDescriptorSet descriptor_sets[] = {
        m_downsample.write_ds[m_common_resources->ping_pong]->handle(),
        m_temporal_reprojection.output_bilinear_read_ds[m_common_resources->ping_pong]->handle()
    };

    vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_downsample.pipeline_layout->handle(), 0, 2, descriptor_sets, 0, nullptr);

    int w = m_recurrent_blur.image->width() / 2;
    int h = m_recurrent_blur.image->height() / 2;

    vkCmdDispatch(cmd_buf->handle(), static_cast<uint32_t>(ceil(float(w) / float(8))), static_cast<uint32_t>(ceil(float(h) / float(8))), 1);

    {
        std::vector<VkMemoryBarrier> memory_barriers = {
            memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
        };

        std::vector<VkImageMemoryBarrier> image_barriers = {
            image_memory_barrier(m_temporal_reprojection.color_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, subresource_range, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
        };

        pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::gaussian_blur(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE("Gaussian Blur", cmd_buf);

    const int NUM_THREADS_X = 8;
    const int NUM_THREADS_Y = 8;

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Vertical
    {
        DW_SCOPED_SAMPLE("Vertical", cmd_buf);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_gaussian_blur.image[0]->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            subresource_range);

        vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_gaussian_blur.pipeline->handle());

        GaussianBlurPushConstants push_constants;

        push_constants.z_buffer_params = m_common_resources->z_buffer_params;
        push_constants.direction       = glm::ivec2(1, 0);
        push_constants.radius          = m_gaussian_blur.blur_radius;

        vkCmdPushConstants(cmd_buf->handle(), m_gaussian_blur.layout->handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

        VkDescriptorSet descriptor_sets[] = {
            m_gaussian_blur.write_ds[0]->handle(),
            m_temporal_reprojection.output_read_ds[m_common_resources->ping_pong]->handle(),
            m_temporal_reprojection.read_ds[m_common_resources->ping_pong]->handle(),
            m_g_buffer->output_ds()->handle()
        };

        vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_gaussian_blur.layout->handle(), 0, 4, descriptor_sets, 0, nullptr);

        vkCmdDispatch(cmd_buf->handle(), static_cast<uint32_t>(ceil(float(m_gaussian_blur.image[0]->width()) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_gaussian_blur.image[0]->height()) / float(NUM_THREADS_Y))), 1);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_gaussian_blur.image[0]->handle(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);
    }

    // Horizontal
    {
        DW_SCOPED_SAMPLE("Horizontal", cmd_buf);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_gaussian_blur.image[1]->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            subresource_range);

        vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_gaussian_blur.pipeline->handle());

        GaussianBlurPushConstants push_constants;

        push_constants.z_buffer_params = m_common_resources->z_buffer_params;
        push_constants.direction       = glm::ivec2(0, 1);
        push_constants.radius          = m_gaussian_blur.blur_radius;

        vkCmdPushConstants(cmd_buf->handle(), m_gaussian_blur.layout->handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

        VkDescriptorSet descriptor_sets[] = {
            m_gaussian_blur.write_ds[1]->handle(),
            m_gaussian_blur.read_ds[0]->handle(),
            m_temporal_reprojection.read_ds[m_common_resources->ping_pong]->handle(),
            m_g_buffer->output_ds()->handle()
        };

        vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_gaussian_blur.layout->handle(), 0, 4, descriptor_sets, 0, nullptr);

        vkCmdDispatch(cmd_buf->handle(), static_cast<uint32_t>(ceil(float(m_gaussian_blur.image[0]->width()) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_gaussian_blur.image[0]->height()) / float(NUM_THREADS_Y))), 1);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_gaussian_blur.image[1]->handle(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void RayTracedAO::recurrent_blur(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE("Recurrent Blur", cmd_buf);

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_recurrent_blur.image->handle(),
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,
        subresource_range);

    vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_recurrent_blur.pipeline->handle());

    RecurrentBlurPushConstants push_constants;

    push_constants.z_buffer_params = m_common_resources->z_buffer_params;
    push_constants.radius          = (float)m_recurrent_blur.blur_radius;
    push_constants.num_frames      = m_common_resources->num_frames;
    push_constants.self_stabilize  = (uint32_t)m_recurrent_blur.self_stabilize;

    vkCmdPushConstants(cmd_buf->handle(), m_recurrent_blur.layout->handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

    VkDescriptorSet descriptor_sets[] = {
        m_recurrent_blur.write_ds->handle(),
        m_temporal_reprojection.output_read_ds[m_common_resources->ping_pong]->handle(),
        m_temporal_reprojection.read_ds[m_common_resources->ping_pong]->handle(),
        m_g_buffer->output_ds()->handle()
    };

    vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_recurrent_blur.layout->handle(), 0, 4, descriptor_sets, 0, nullptr);

    const int NUM_THREADS_X = 32;
    const int NUM_THREADS_Y = 32;

    vkCmdDispatch(cmd_buf->handle(), static_cast<uint32_t>(ceil(float(m_recurrent_blur.image->width()) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_recurrent_blur.image->height()) / float(NUM_THREADS_Y))), 1);

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_recurrent_blur.image->handle(),
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        subresource_range);
}

// -----------------------------------------------------------------------------------------------------------------------------------