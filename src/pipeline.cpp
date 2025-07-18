// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "pipeline.h"

#include "layer_shader_type.h"
#include "mat.h"
#include "pipelinecache.h"
#include "option.h"

#if __ANDROID_API__ >= 26
#include <android/hardware_buffer.h>
#endif // __ANDROID_API__ >= 26

namespace ncnn {

#if NCNN_VULKAN
class PipelinePrivate
{
public:
    VkShaderModule shader_module;
    VkDescriptorSetLayout descriptorset_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkDescriptorUpdateTemplateKHR descriptor_update_template;

    ShaderInfo shader_info;

    uint32_t local_size_x;
    uint32_t local_size_y;
    uint32_t local_size_z;
    uint32_t subgroup_size;
};

Pipeline::Pipeline(const VulkanDevice* _vkdev)
    : vkdev(_vkdev), d(new PipelinePrivate)
{
    d->shader_module = 0;
    d->descriptorset_layout = 0;
    d->pipeline_layout = 0;
    d->pipeline = 0;
    d->descriptor_update_template = 0;

    d->local_size_x = 1;
    d->local_size_y = 1;
    d->local_size_z = 1;
    d->subgroup_size = vkdev->info.subgroup_size();
}

Pipeline::~Pipeline()
{
    delete d;
}

Pipeline::Pipeline(const Pipeline&)
    : d(0)
{
}

Pipeline& Pipeline::operator=(const Pipeline&)
{
    return *this;
}

void Pipeline::set_optimal_local_size_xyz(int w, int h, int c)
{
    set_optimal_local_size_xyz(Mat(w, h, c, (void*)0));
}

void Pipeline::set_optimal_local_size_xyz(const Mat& local_size_xyz)
{
    int w = local_size_xyz.w;
    int h = local_size_xyz.h;
    int c = local_size_xyz.c;

    if (w == 0 && h == 0 && c == 0)
    {
        // fallback to the common and safe 4x4x4
        w = 4;
        h = 4;
        c = 4;
    }

    w = std::min(w, (int)vkdev->info.max_workgroup_size_x());
    h = std::min(h, (int)vkdev->info.max_workgroup_size_y());
    c = std::min(c, (int)vkdev->info.max_workgroup_size_z());

    if (w * h * c <= (int)vkdev->info.max_workgroup_invocations())
    {
        return set_local_size_xyz(w, h, c);
    }

    int max_local_size_xy = (int)vkdev->info.max_workgroup_invocations() / c;

    int wh_max = std::max(1, (int)sqrt(max_local_size_xy));
    while (w * h >= wh_max)
    {
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }

    set_local_size_xyz(w, h, c);
}

void Pipeline::set_subgroup_size(uint32_t subgroup_size)
{
    // assert subgroup_size be power of two
    subgroup_size = std::max(subgroup_size, vkdev->info.min_subgroup_size());
    subgroup_size = std::min(subgroup_size, vkdev->info.max_subgroup_size());

    d->subgroup_size = subgroup_size;
}

static int count_trailing_zeros(unsigned int v)
{
    int cnt = 0;
    while ((v & 1) == 0)
    {
        cnt++;
        v >>= 1;
    }
    return cnt;
}

// round up v to the next multiple of 2^k
static unsigned int round_up_pow2_mul(unsigned int v, int k)
{
    unsigned int m = 1u << k;
    return ((v + m - 1) / m) * m;
}

// adjust x, y, z so that new x * y * z is a multiple of size (size must be a power of two), and new x, y, z are no less than the inputs
// new values do not have to be integer multiples of the originals
// minimize the total increment (x'-x)+(y'-y)+(z'-z)
// additional constraint: if original y is 1, prefer not to adjust y; if original z is 1, prefer not to adjust z
static void adjust_xyz(int& x, int& y, int& z, const int subgroup_size)
{
    if (x * y * z % subgroup_size == 0)
        return;

    int target_n = 0;
    {
        while ((1 << target_n) != subgroup_size)
            target_n++;
    }

    // subgroup shall usually be 4 ~ 128, sanitize the max possible size
    target_n = std::min(target_n, 10);

    const int tx = count_trailing_zeros((unsigned int)x);
    const int ty = count_trailing_zeros((unsigned int)y);
    const int tz = count_trailing_zeros((unsigned int)z);
    const int tn = tx + ty + tz;

    const int need = target_n - tn;

    if (z == 1)
    {
        if (y == 1)
        {
            // adjust x only
            x = round_up_pow2_mul((unsigned int)x, target_n);
        }
        else if (x == 1)
        {
            // adjust y only
            y = round_up_pow2_mul((unsigned int)y, target_n);
        }
        else
        {
            // adjust x and y
            y = round_up_pow2_mul((unsigned int)y, ty + need / 2);
            x = round_up_pow2_mul((unsigned int)x, tx + need - need / 2);
        }
    }
    else if (y == 1)
    {
        if (x == 1)
        {
            // adjust z only
            z = round_up_pow2_mul((unsigned int)z, target_n);
        }
        else
        {
            // adjust x and z
            z = round_up_pow2_mul((unsigned int)z, tz + need / 2);
            x = round_up_pow2_mul((unsigned int)x, tx + need - need / 2);
        }
    }
    else if (x == 1)
    {
        // adjust y and z
        z = round_up_pow2_mul((unsigned int)z, tz + need / 2);
        y = round_up_pow2_mul((unsigned int)y, ty + need - need / 2);
    }
    else
    {
        // adjust x y z
        z = round_up_pow2_mul((unsigned int)z, tz + need / 3);
        y = round_up_pow2_mul((unsigned int)y, ty + (need - need / 3) / 2);
        x = round_up_pow2_mul((unsigned int)x, tx + need - (need - need / 3) / 2);
    }
}

void Pipeline::set_local_size_xyz(int w, int h, int c)
{
    // dispatch at least one subgroup
    // make local size be multiple of subgroup size
    // and metal is unhappy with arbitrary local size anyway
    adjust_xyz(w, h, c, d->subgroup_size);

    d->local_size_x = w;
    d->local_size_y = h;
    d->local_size_z = c;

    // NCNN_LOGE("local size = %d %d %d", local_size_x, local_size_y, local_size_z);
}

int Pipeline::create(const uint32_t* spv_data, size_t spv_data_size, const std::vector<vk_specialization_type>& specializations)
{
    const PipelineCache* pipeline_cache = vkdev->get_pipeline_cache();

    // get from pipeline cache
    return pipeline_cache->get_pipeline(spv_data, spv_data_size, specializations, d->local_size_x, d->local_size_y, d->local_size_z, d->subgroup_size,
                                        &d->shader_module, &d->descriptorset_layout, &d->pipeline_layout, &d->pipeline, &d->descriptor_update_template,
                                        d->shader_info);
}

int Pipeline::create(int shader_type_index, const Option& opt, const std::vector<vk_specialization_type>& specializations)
{
    const PipelineCache* pipeline_cache = opt.pipeline_cache ? opt.pipeline_cache : vkdev->get_pipeline_cache();

    // get from pipeline cache
    return pipeline_cache->get_pipeline(shader_type_index, opt, specializations, d->local_size_x, d->local_size_y, d->local_size_z, d->subgroup_size,
                                        &d->shader_module, &d->descriptorset_layout, &d->pipeline_layout, &d->pipeline, &d->descriptor_update_template,
                                        d->shader_info);
}

VkShaderModule Pipeline::shader_module() const
{
    return d->shader_module;
}

VkDescriptorSetLayout Pipeline::descriptorset_layout() const
{
    return d->descriptorset_layout;
}

VkPipelineLayout Pipeline::pipeline_layout() const
{
    return d->pipeline_layout;
}

VkPipeline Pipeline::pipeline() const
{
    return d->pipeline;
}

VkDescriptorUpdateTemplateKHR Pipeline::descriptor_update_template() const
{
    return d->descriptor_update_template;
}

const ShaderInfo& Pipeline::shader_info() const
{
    return d->shader_info;
}

uint32_t Pipeline::local_size_x() const
{
    return d->local_size_x;
}

uint32_t Pipeline::local_size_y() const
{
    return d->local_size_y;
}

uint32_t Pipeline::local_size_z() const
{
    return d->local_size_z;
}

void Pipeline::set_shader_module(VkShaderModule shader_module)
{
    d->shader_module = shader_module;
}

void Pipeline::set_descriptorset_layout(VkDescriptorSetLayout descriptorset_layout)
{
    d->descriptorset_layout = descriptorset_layout;
}

void Pipeline::set_pipeline_layout(VkPipelineLayout pipeline_layout)
{
    d->pipeline_layout = pipeline_layout;
}

void Pipeline::set_pipeline(VkPipeline pipeline)
{
    d->pipeline = pipeline;
}

void Pipeline::set_descriptor_update_template(VkDescriptorUpdateTemplateKHR descriptor_update_template)
{
    d->descriptor_update_template = descriptor_update_template;
}

void Pipeline::set_shader_info(const ShaderInfo& shader_info)
{
    d->shader_info = shader_info;
}

#if NCNN_PLATFORM_API
#if __ANDROID_API__ >= 26
ImportAndroidHardwareBufferPipeline::ImportAndroidHardwareBufferPipeline(const VulkanDevice* _vkdev)
    : Pipeline(_vkdev)
{
    sampler = 0;
}

ImportAndroidHardwareBufferPipeline::~ImportAndroidHardwareBufferPipeline()
{
    destroy();
}

int ImportAndroidHardwareBufferPipeline::create(VkAndroidHardwareBufferImageAllocator* ahb_im_allocator, int _type_to, int _rotate_from, const Option& opt)
{
    int target_width;
    int target_height;

    if (rotate_from < 5) // 1 2 3 4
    {
        target_width = ahb_im_allocator->width();
        target_height = ahb_im_allocator->height();
    }
    else // 5 6 7 8
    {
        target_width = ahb_im_allocator->height();
        target_height = ahb_im_allocator->width();
    }

    return create(ahb_im_allocator, _type_to, _rotate_from, target_width, target_height, opt);
}

int ImportAndroidHardwareBufferPipeline::create(VkAndroidHardwareBufferImageAllocator* ahb_im_allocator, int _type_to, int _rotate_from, int target_width, int target_height, const Option& opt)
{
    int w = ahb_im_allocator->width();
    int h = ahb_im_allocator->height();

    type_to = _type_to;
    rotate_from = _rotate_from;

    need_resize = false;
    if (rotate_from < 5) // 1 2 3 4
    {
        if (target_width != w || target_height != h)
            need_resize = true;
    }
    else // 5 6 7 8
    {
        if (target_width != h || target_height != w)
            need_resize = true;
    }

    //     if (type_to == 1 || type_to == 2)
    //     {
    //         outc = 3;
    //         out_elemsize = vkdev->info.support_fp16_storage() && opt.use_fp16_storage ? 2u : 4u;
    //         out_elempack = 1;
    //     }
    //     else if (type_to == 3)
    //     {
    //         outc = 1;
    //         out_elemsize = vkdev->info.support_fp16_storage() && opt.use_fp16_storage ? 2u : 4u;
    //         out_elempack = 1;
    //     }
    //     else // if (type_to == 4 || type_to == 5)
    //     {
    //         outc = 1;
    //         out_elemsize = ((vkdev->info.support_fp16_packed() && opt.use_fp16_packed) || (vkdev->info.support_fp16_storage() && opt.use_fp16_storage)) ? 8u : 16u;
    //         out_elempack = 4;
    //     }

    set_local_size_xyz(8, 8, 1);

    std::vector<vk_specialization_type> specializations(7);
    specializations[0].i = ahb_im_allocator->width();
    specializations[1].i = ahb_im_allocator->height();
    specializations[2].i = target_width;
    specializations[3].i = target_height;
    specializations[4].i = type_to;
    specializations[5].i = rotate_from;
    specializations[6].i = need_resize;

    create_shader_module(opt);

    const ShaderInfo& _shader_info = shader_info();

    if ((int)specializations.size() != _shader_info.specialization_count)
    {
        NCNN_LOGE("pipeline convert_ycbcr specialization count mismatch, expect %d but got %d", _shader_info.specialization_count, (int)specializations.size());
        return -1;
    }

    create_sampler(ahb_im_allocator);

    create_descriptorset_layout();

    VkPipelineLayout pipeline_layout = 0;
    VkPipeline pipeline = 0;
    VkDescriptorUpdateTemplateKHR descriptor_update_template = 0;

    vkdev->create_pipeline_layout(_shader_info.push_constant_count, descriptorset_layout(), &pipeline_layout);

    vkdev->create_pipeline(shader_module(), pipeline_layout, specializations, vkdev->info.subgroup_size(), &pipeline);

    if (vkdev->info.support_VK_KHR_descriptor_update_template())
    {
        vkdev->create_descriptor_update_template(_shader_info.binding_count, _shader_info.binding_types, descriptorset_layout(), pipeline_layout, &descriptor_update_template);
    }

    set_pipeline_layout(pipeline_layout);
    set_pipeline(pipeline);
    set_descriptor_update_template(descriptor_update_template);

    return 0;
}

void ImportAndroidHardwareBufferPipeline::destroy()
{
    if (sampler)
    {
        vkDestroySampler(vkdev->vkdevice(), sampler, 0);
        sampler = 0;
    }
}

int ImportAndroidHardwareBufferPipeline::create_shader_module(const Option& opt)
{
    int shader_type_index = LayerShaderType::convert_ycbcr;

    std::vector<uint32_t> spirv;
    int retc = compile_spirv_module(shader_type_index, opt, spirv);
    if (retc != 0)
    {
        NCNN_LOGE("compile_spirv_module failed %d", retc);
        return -1;
    }

    const uint32_t* spv_data = spirv.data();
    size_t spv_data_size = spirv.size() * 4;

    ShaderInfo shader_info;
    int ret = resolve_shader_info(spv_data, spv_data_size, shader_info);
    if (ret != 0)
    {
        NCNN_LOGE("resolve_shader_info failed %d", ret);
        return -1;
    }

    set_shader_info(shader_info);

    VkShaderModule shader_module = vkdev->compile_shader_module(spv_data, spv_data_size, local_size_x(), local_size_y(), local_size_z());
    set_shader_module(shader_module);

    return 0;
}

int ImportAndroidHardwareBufferPipeline::create_sampler(VkAndroidHardwareBufferImageAllocator* ahb_im_allocator)
{
    VkResult ret;

    VkExternalFormatANDROID externalFormatANDROID;
    externalFormatANDROID.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    externalFormatANDROID.pNext = 0;
    externalFormatANDROID.externalFormat = ahb_im_allocator->external_format();

    VkSamplerYcbcrConversionInfoKHR samplerYcbcrConversionInfo;
    samplerYcbcrConversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO_KHR;
    samplerYcbcrConversionInfo.pNext = &externalFormatANDROID;
    samplerYcbcrConversionInfo.conversion = ahb_im_allocator->samplerYcbcrConversion;

    VkSamplerCreateInfo samplerCreateInfo;
    samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCreateInfo.pNext = &samplerYcbcrConversionInfo;
    samplerCreateInfo.magFilter = need_resize ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerCreateInfo.minFilter = need_resize ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCreateInfo.mipLodBias = 0.0f;
    samplerCreateInfo.anisotropyEnable = VK_FALSE;
    samplerCreateInfo.maxAnisotropy = 1;
    samplerCreateInfo.compareEnable = VK_FALSE;
    samplerCreateInfo.compareOp = VK_COMPARE_OP_NEVER;
    samplerCreateInfo.minLod = 0.0f;
    samplerCreateInfo.maxLod = 0.0f;
    samplerCreateInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; //VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; FIXME
    samplerCreateInfo.unnormalizedCoordinates = VK_TRUE;                     //VK_FALSE; FIXME ?

    ret = vkCreateSampler(vkdev->vkdevice(), &samplerCreateInfo, 0, &sampler);
    if (ret != VK_SUCCESS)
    {
        NCNN_LOGE("vkCreateSampler failed %d", ret);
        return -1;
    }

    return 0;
}

int ImportAndroidHardwareBufferPipeline::create_descriptorset_layout()
{
    VkDescriptorSetLayoutBinding descriptorSetLayoutBindings[3];
    descriptorSetLayoutBindings[0].binding = 0;
    descriptorSetLayoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorSetLayoutBindings[0].descriptorCount = 1;
    descriptorSetLayoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    descriptorSetLayoutBindings[0].pImmutableSamplers = &sampler;
    descriptorSetLayoutBindings[1].binding = 1;
    descriptorSetLayoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorSetLayoutBindings[1].descriptorCount = 1;
    descriptorSetLayoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    descriptorSetLayoutBindings[1].pImmutableSamplers = 0;
    descriptorSetLayoutBindings[2].binding = 2;
    descriptorSetLayoutBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorSetLayoutBindings[2].descriptorCount = 1;
    descriptorSetLayoutBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    descriptorSetLayoutBindings[2].pImmutableSamplers = 0;

    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
    descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutCreateInfo.pNext = 0;
    descriptorSetLayoutCreateInfo.flags = 0;
    descriptorSetLayoutCreateInfo.bindingCount = 3;
    descriptorSetLayoutCreateInfo.pBindings = descriptorSetLayoutBindings;

    if (vkdev->info.support_VK_KHR_push_descriptor())
    {
        descriptorSetLayoutCreateInfo.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    }

    VkDescriptorSetLayout descriptorset_layout = 0;
    VkResult ret = vkCreateDescriptorSetLayout(vkdev->vkdevice(), &descriptorSetLayoutCreateInfo, 0, &descriptorset_layout);
    if (ret != VK_SUCCESS)
    {
        NCNN_LOGE("vkCreateDescriptorSetLayout failed %d", ret);
        return -1;
    }

    set_descriptorset_layout(descriptorset_layout);

    return 0;
}
#endif // __ANDROID_API__ >= 26
#endif // NCNN_PLATFORM_API

#endif // NCNN_VULKAN

} // namespace ncnn
