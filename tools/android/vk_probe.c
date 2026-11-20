/* Device-side Vulkan probe (M3 step 0 / DXVK feature policy).
 *
 * Runs as a plain arm64 executable on the phone (adb shell), no Activity, so it
 * can answer every question that does not need a surface: instance/device
 * version, driver identity, extensions, every VkPhysicalDeviceFeatures field,
 * the 1.1/1.2/1.3 feature structs DXVK cares about, format support for the
 * D3D9 formats Korra uses (X8R8G8B8, A8R8G8B8, D24S8, BC1/BC2/BC3, L8, ...),
 * queue families and memory heaps.
 *
 * Present modes need an ANativeWindow and are reported by the app-side probe
 * (src/host_android/), not here.
 *
 * Build + run: scripts\android_probe.ps1 -Run vk_probe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#define ARRAYSZ(a) ((unsigned)(sizeof(a) / sizeof((a)[0])))

static const char *vresult(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: return "VK_ERROR_OTHER";
    }
}

#define FEAT(name) { #name, (unsigned)f->name }
static void dump_features(const VkPhysicalDeviceFeatures *f) {
    struct { const char *n; unsigned v; } rows[] = {
        FEAT(robustBufferAccess), FEAT(fullDrawIndexUint32), FEAT(imageCubeArray),
        FEAT(independentBlend), FEAT(geometryShader), FEAT(tessellationShader),
        FEAT(sampleRateShading), FEAT(dualSrcBlend), FEAT(logicOp),
        FEAT(multiDrawIndirect), FEAT(drawIndirectFirstInstance), FEAT(depthClamp),
        FEAT(depthBiasClamp), FEAT(fillModeNonSolid), FEAT(depthBounds),
        FEAT(wideLines), FEAT(largePoints), FEAT(alphaToOne), FEAT(multiViewport),
        FEAT(samplerAnisotropy), FEAT(textureCompressionETC2),
        FEAT(textureCompressionASTC_LDR), FEAT(textureCompressionBC),
        FEAT(occlusionQueryPrecise), FEAT(pipelineStatisticsQuery),
        FEAT(vertexPipelineStoresAndAtomics), FEAT(fragmentStoresAndAtomics),
        FEAT(shaderTessellationAndGeometryPointSize),
        FEAT(shaderImageGatherExtended), FEAT(shaderStorageImageExtendedFormats),
        FEAT(shaderStorageImageMultisample), FEAT(shaderStorageImageReadWithoutFormat),
        FEAT(shaderStorageImageWriteWithoutFormat),
        FEAT(shaderUniformBufferArrayDynamicIndexing),
        FEAT(shaderSampledImageArrayDynamicIndexing),
        FEAT(shaderStorageBufferArrayDynamicIndexing),
        FEAT(shaderStorageImageArrayDynamicIndexing), FEAT(shaderClipDistance),
        FEAT(shaderCullDistance), FEAT(shaderFloat64), FEAT(shaderInt64),
        FEAT(shaderInt16), FEAT(shaderResourceResidency),
        FEAT(shaderResourceMinLod), FEAT(sparseBinding),
        FEAT(sparseResidencyBuffer), FEAT(sparseResidencyImage2D),
        FEAT(sparseResidencyImage3D), FEAT(sparseResidency2Samples),
        FEAT(sparseResidency4Samples), FEAT(sparseResidency8Samples),
        FEAT(sparseResidency16Samples), FEAT(sparseResidencyAliased),
        FEAT(variableMultisampleRate), FEAT(inheritedQueries),
    };
    unsigned i;
    for (i = 0; i < ARRAYSZ(rows); i++) printf("  feature %-46s %u\n", rows[i].n, rows[i].v);
}

static void fmtpro(char *out, size_t cap, uint32_t p) {
    out[0] = 0;
#define ADD(b, s) if ((p & (b)) && strlen(out) + strlen(s) + 2 < cap) { strcat(out, s); strcat(out, " "); }
    ADD(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "SAMPLED")
    ADD(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "STORAGE")
    ADD(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "COLOR_ATTACH")
    ADD(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "DEPTH_ATTACH")
    ADD(VK_FORMAT_FEATURE_BLIT_SRC_BIT, "BLIT_SRC")
    ADD(VK_FORMAT_FEATURE_BLIT_DST_BIT, "BLIT_DST")
    ADD(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, "FILTER_LINEAR")
#undef ADD
    if (!out[0]) strncpy(out, "-", cap), out[cap - 1] = 0;
}

static void dump_format(VkPhysicalDevice pd, VkFormat fmt, const char *label) {
    VkFormatProperties p;
    char a[160], b[160];
    vkGetPhysicalDeviceFormatProperties(pd, fmt, &p);
    fmtpro(a, sizeof a, p.optimalTilingFeatures);
    fmtpro(b, sizeof b, p.linearTilingFeatures);
    printf("  fmt %-28s opt: %s| lin: %s|\n", label, a, b);
}

int main(void) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r;
    uint32_t n = 0, i;

    app.pApplicationName = "korra-vk-probe";
    app.apiVersion = VK_API_VERSION_1_1;
    ci.pApplicationInfo = &app;

    r = vkCreateInstance(&ci, NULL, &inst);
    if (r != VK_SUCCESS) {
        printf("FATAL vkCreateInstance(%s)\n", vresult(r));
        return 1;
    }
    printf("instance api=%u.%u.%u\n",
           VK_VERSION_MAJOR(app.apiVersion), VK_VERSION_MINOR(app.apiVersion),
           VK_VERSION_PATCH(app.apiVersion));

    vkEnumerateInstanceLayerProperties(&n, NULL);
    printf("instance layers: %u\n", n);

    n = 0; vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    {
        VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
        vkEnumerateInstanceExtensionProperties(NULL, &n, e);
        printf("instance extensions: %u\n", n);
        for (i = 0; i < n; i++) printf("  iext %s (rev %u)\n", e[i].extensionName, e[i].specVersion);
        free(e);
    }

    n = 0;
    r = vkEnumeratePhysicalDevices(inst, &n, NULL);
    printf("physical devices: %u (%s)\n", n, vresult(r));
    if (n == 0) return 1;
    {
        VkPhysicalDevice *pd = calloc(n, sizeof(*pd));
        vkEnumeratePhysicalDevices(inst, &n, pd);
        for (i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p;
            VkPhysicalDeviceDriverProperties drv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            VkPhysicalDeviceFeatures f;
            uint32_t qn = 0, en = 0, j;

            vkGetPhysicalDeviceProperties(pd[i], &p);
            p2.pNext = &drv;
            vkGetPhysicalDeviceProperties2(pd[i], &p2);
            vkGetPhysicalDeviceFeatures(pd[i], &f);
            printf("\n[device %u] %s\n", i, p.deviceName);
            printf("  api=%u.%u.%u driverVersion=0x%08X (raw) vendor=0x%04X device=0x%04X type=%u\n",
                   VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                   VK_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID,
                   p.deviceID, p.deviceType);
            if (p.vendorID == 0x1002) { /* AMD convention, Xclipse is RDNA-derived */
                printf("  driver (AMD layout) %u.%u.%u\n",
                       VK_VERSION_MAJOR(p.driverVersion),
                       VK_VERSION_MINOR(p.driverVersion), VK_VERSION_PATCH(p.driverVersion));
            } else {
                printf("  driver (VK layout) %u.%u.%u\n",
                       (p.driverVersion >> 22) & 0x3FF, (p.driverVersion >> 12) & 0x3FF,
                       p.driverVersion & 0xFFF);
            }
            printf("  driverID=%u driverInfo=%s\n", drv.driverID, drv.driverInfo);
            printf("  limits: maxImage2D=%u maxFB=%ux%u maxColorAtt=%u maxSamplers=%u "
                   "maxAniso=%.2f maxVertexAttribs=%u maxBindlessSampled(perstage)=%u "
                   "timestampPeriod=%.2f maxClipDistances=%u maxCullDistances=%u maxViewports=%u\n",
                   p.limits.maxImageDimension2D, p.limits.maxFramebufferWidth,
                   p.limits.maxFramebufferHeight, p.limits.maxColorAttachments,
                   p.limits.maxPerStageDescriptorSamplers, p.limits.maxSamplerAnisotropy,
                   p.limits.maxVertexInputAttributes,
                   p.limits.maxPerStageDescriptorSampledImages, p.limits.timestampPeriod,
                   p.limits.maxClipDistances, p.limits.maxCullDistances, p.limits.maxViewports);

            dump_features(&f);

            /* Vulkan 1.1/1.2/1.3 features that gate DXVK paths. */
            {
                VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
                VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
                VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
                VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
                f2.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13;
                vkGetPhysicalDeviceFeatures2(pd[i], &f2);
                printf("  vk11: multiview=%u shaderDrawParameters=%u samplerYcbcr=%u\n",
                       f11.multiview, f11.shaderDrawParameters, f11.samplerYcbcrConversion);
                printf("  vk12: timelineSemaphore=%u bufferDeviceAddress=%u "
                       "descriptorIndexing=%u shaderSampledImageArrayNonUniform=%u "
                       "shaderStorageBufferArrayNonUniform=%u drawIndirectCount=%u "
                       "scalarBlockLayout=%u uniformBufferStandardLayout=%u "
                       "hostQueryReset=%u\n",
                       f12.timelineSemaphore, f12.bufferDeviceAddress,
                       f12.descriptorIndexing, f12.shaderSampledImageArrayNonUniformIndexing,
                       f12.shaderStorageBufferArrayNonUniformIndexing, f12.drawIndirectCount,
                       f12.scalarBlockLayout, f12.uniformBufferStandardLayout,
                       f12.hostQueryReset);
                printf("  vk13: dynamicRendering=%u synchronization2=%u "
                       "maintenance4=%u computeFullSubgroups=%u\n",
                       f13.dynamicRendering, f13.synchronization2,
                       f13.maintenance4, f13.computeFullSubgroups);
            }

            /* Device extensions DXVK looks for. */
            vkEnumerateDeviceExtensionProperties(pd[i], NULL, &en, NULL);
            {
                VkExtensionProperties *e = calloc(en ? en : 1, sizeof(*e));
                static const char *want[] = {
                    "VK_KHR_swapchain", "VK_KHR_maintenance1", "VK_KHR_maintenance2",
                    "VK_KHR_maintenance3", "VK_KHR_image_format_list",
                    "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
                    "VK_KHR_create_renderpass2", "VK_KHR_depth_stencil_resolve",
                    "VK_KHR_shader_draw_parameters", "VK_KHR_draw_indirect_count",
                    "VK_EXT_transform_feedback", "VK_EXT_vertex_attribute_divisor",
                    "VK_EXT_depth_clip_enable", "VK_EXT_custom_border_color",
                    "VK_EXT_robustness2", "VK_EXT_memory_budget",
                    "VK_EXT_shader_viewport_index_layer", "VK_KHR_pipeline_library",
                    "VK_EXT_swapchain_colorspace", "VK_ANDROID_external_memory_android_hardware_buffer",
                };
                uint32_t w;
                vkEnumerateDeviceExtensionProperties(pd[i], NULL, &en, e);
                printf("  device extensions: %u\n", en);
                for (w = 0; w < ARRAYSZ(want); w++) {
                    uint32_t k; int hit = 0;
                    for (k = 0; k < en; k++)
                        if (strcmp(e[k].extensionName, want[w]) == 0) { hit = 1; break; }
                    printf("    %-52s %s\n", want[w], hit ? "yes" : "NO");
                }
                free(e);
            }

            /* Queue families. */
            vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &qn, NULL);
            {
                VkQueueFamilyProperties *q = calloc(qn ? qn : 1, sizeof(*q));
                vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &qn, q);
                for (j = 0; j < qn; j++) {
                    printf("  qfam %u flags=0x%X count=%u tsBits=%u granularity=%ux%ux%u\n",
                           j, q[j].queueFlags, q[j].queueCount,
                           q[j].timestampValidBits,
                           q[j].minImageTransferGranularity.width,
                           q[j].minImageTransferGranularity.height,
                           q[j].minImageTransferGranularity.depth);
                }
                free(q);
            }

            /* Memory. */
            {
                VkPhysicalDeviceMemoryProperties m;
                vkGetPhysicalDeviceMemoryProperties(pd[i], &m);
                printf("  memory heaps=%u types=%u\n", m.memoryHeapCount, m.memoryTypeCount);
                for (j = 0; j < m.memoryHeapCount; j++)
                    printf("    heap %u size=%lluMB flags=0x%X\n", j,
                           (unsigned long long)(m.memoryHeaps[j].size >> 20),
                           m.memoryHeaps[j].flags);
                for (j = 0; j < m.memoryTypeCount; j++)
                    printf("    type %u heap=%u flags=0x%X\n", j,
                           m.memoryTypes[j].heapIndex, m.memoryTypes[j].propertyFlags);
            }

            /* The formats Korra's D3D9 path needs. */
            dump_format(pd[i], VK_FORMAT_B8G8R8A8_UNORM, "X8R8G8B8/A8R8G8B8");
            dump_format(pd[i], VK_FORMAT_B8G8R8A8_SRGB, "X8R8G8B8_sRGB");
            dump_format(pd[i], VK_FORMAT_A8B8G8R8_UNORM_PACK32, "A8R8G8B8 packed");
            dump_format(pd[i], VK_FORMAT_D24_UNORM_S8_UINT, "D24S8");
            dump_format(pd[i], VK_FORMAT_D32_SFLOAT, "D32F");
            dump_format(pd[i], VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "DXT1 (BC1)");
            dump_format(pd[i], VK_FORMAT_BC2_UNORM_BLOCK, "DXT3 (BC2)");
            dump_format(pd[i], VK_FORMAT_BC3_UNORM_BLOCK, "DXT5 (BC3)");
            dump_format(pd[i], VK_FORMAT_R8_UNORM, "L8");
            dump_format(pd[i], VK_FORMAT_B5G6R5_UNORM_PACK16, "R5G6B5");
            dump_format(pd[i], VK_FORMAT_B4G4R4A4_UNORM_PACK16, "A4R4G4B4");
            dump_format(pd[i], VK_FORMAT_R16G16B16A16_SFLOAT, "A16B16G16R16F");
            dump_format(pd[i], VK_FORMAT_R32_SFLOAT, "R32F");
        }
        free(pd);
    }
    vkDestroyInstance(inst, NULL);
    printf("\nPROBE OK\n");
    return 0;
}
