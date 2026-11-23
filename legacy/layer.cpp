#include "vk_dispatch.hpp"
#include "config.hpp"

#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <iostream>

// Include do header com bytecode SPIR-V gerado no build
#include "shader_spv.h"

namespace chroma {

struct InstanceData;
struct DeviceData;

struct SwapchainImageResource {
    VkImage image{VK_NULL_HANDLE};
    VkImageView storageView{VK_NULL_HANDLE};
    VkDescriptorSet descSet{VK_NULL_HANDLE};
    VkCommandBuffer cmdBuffer{VK_NULL_HANDLE};
    VkSemaphore postComputeSemaphore{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};
    bool fence_in_flight{false};
};

struct SwapchainData {
    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    DeviceData* deviceData{nullptr};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{0, 0};
    bool storage_supported{false};
    std::vector<SwapchainImageResource> images;
};

struct InstanceData {
    VkInstance instance{VK_NULL_HANDLE};
    PFN_vkGetInstanceProcAddr next_gipa{nullptr};
    PFN_vkDestroyInstance vkDestroyInstance{nullptr};
    PFN_vkCreateDevice vkCreateDevice{nullptr};
    PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties{nullptr};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties{nullptr};
};

struct DeviceData {
    VkDevice device{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    InstanceData* instData{nullptr};
    uint32_t queueFamilyIndex{0};

    // Tabela de despacho do dispositivo
    PFN_vkGetDeviceProcAddr next_gdpa{nullptr};
    PFN_vkDestroyDevice vkDestroyDevice{nullptr};
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR{nullptr};
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR{nullptr};
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR{nullptr};
    PFN_vkQueuePresentKHR vkQueuePresentKHR{nullptr};

    // Funções Vulkan utilizadas para o pipeline de compute
    PFN_vkCreateShaderModule vkCreateShaderModule{nullptr};
    PFN_vkDestroyShaderModule vkDestroyShaderModule{nullptr};
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout{nullptr};
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout{nullptr};
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool{nullptr};
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool{nullptr};
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets{nullptr};
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets{nullptr};
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout{nullptr};
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout{nullptr};
    PFN_vkCreateComputePipelines vkCreateComputePipelines{nullptr};
    PFN_vkDestroyPipeline vkDestroyPipeline{nullptr};
    PFN_vkCreateImageView vkCreateImageView{nullptr};
    PFN_vkDestroyImageView vkDestroyImageView{nullptr};
    PFN_vkCreateCommandPool vkCreateCommandPool{nullptr};
    PFN_vkDestroyCommandPool vkDestroyCommandPool{nullptr};
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers{nullptr};
    PFN_vkResetCommandBuffer vkResetCommandBuffer{nullptr};
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer{nullptr};
    PFN_vkEndCommandBuffer vkEndCommandBuffer{nullptr};
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier{nullptr};
    PFN_vkCmdBindPipeline vkCmdBindPipeline{nullptr};
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets{nullptr};
    PFN_vkCmdPushConstants vkCmdPushConstants{nullptr};
    PFN_vkCmdDispatch vkCmdDispatch{nullptr};
    PFN_vkQueueSubmit vkQueueSubmit{nullptr};
    PFN_vkCreateSemaphore vkCreateSemaphore{nullptr};
    PFN_vkDestroySemaphore vkDestroySemaphore{nullptr};
    PFN_vkCreateFence vkCreateFence{nullptr};
    PFN_vkDestroyFence vkDestroyFence{nullptr};
    PFN_vkWaitForFences vkWaitForFences{nullptr};
    PFN_vkResetFences vkResetFences{nullptr};
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle{nullptr};

    // Recursos do pipeline de Compute da camada Chroma
    bool pipeline_initialized{false};
    VkShaderModule shaderModule{VK_NULL_HANDLE};
    VkDescriptorSetLayout descSetLayout{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    VkPipeline computePipeline{VK_NULL_HANDLE};
    VkDescriptorPool descPool{VK_NULL_HANDLE};
    VkCommandPool commandPool{VK_NULL_HANDLE};

    bool init_chroma_pipeline() {
        if (pipeline_initialized) return true;

        // 1. Shader Module
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = sizeof(chroma_shader_spv);
        moduleInfo.pCode = reinterpret_cast<const uint32_t*>(chroma_shader_spv);
        if (vkCreateShaderModule(device, &moduleInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            std::cerr << "[chroma-layer] Erro: Falha ao criar VkShaderModule!" << std::endl;
            return false;
        }

        // 2. Descriptor Set Layout
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout) != VK_SUCCESS) {
            std::cerr << "[chroma-layer] Erro: Falha ao criar VkDescriptorSetLayout!" << std::endl;
            return false;
        }

        // 3. Pipeline Layout (com Push Constants para latência zero)
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(ChromaPushConstants);

        VkPipelineLayoutCreateInfo pipeLayoutInfo{};
        pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeLayoutInfo.setLayoutCount = 1;
        pipeLayoutInfo.pSetLayouts = &descSetLayout;
        pipeLayoutInfo.pushConstantRangeCount = 1;
        pipeLayoutInfo.pPushConstantRanges = &pcRange;
        if (vkCreatePipelineLayout(device, &pipeLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            std::cerr << "[chroma-layer] Erro: Falha ao criar VkPipelineLayout!" << std::endl;
            return false;
        }

        // 4. Compute Pipeline
        VkComputePipelineCreateInfo computePipeInfo{};
        computePipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        computePipeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        computePipeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computePipeInfo.stage.module = shaderModule;
        computePipeInfo.stage.pName = "main";
        computePipeInfo.layout = pipelineLayout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &computePipeInfo, nullptr, &computePipeline) != VK_SUCCESS) {
            std::cerr << "[chroma-layer] Erro: Falha ao criar VkComputePipeline!" << std::endl;
            return false;
        }

        // 5. Descriptor Pool
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = 64;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 64;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool) != VK_SUCCESS) {
            std::cerr << "[chroma-layer] Erro: Falha ao criar VkDescriptorPool!" << std::endl;
            return false;
        }

        // 6. Command Pool
        VkCommandPoolCreateInfo cmdPoolInfo{};
        cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cmdPoolInfo.queueFamilyIndex = queueFamilyIndex;
        cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            std::cerr << "[chroma-layer] Erro: Falha ao criar VkCommandPool!" << std::endl;
            return false;
        }

        pipeline_initialized = true;
        std::cerr << "[chroma-layer] Pipeline Compute inicializado com sucesso no dispositivo Vulkan!" << std::endl;
        return true;
    }

    void cleanup_pipeline() {
        if (!pipeline_initialized) return;
        if (vkDeviceWaitIdle) vkDeviceWaitIdle(device);

        if (descPool) vkDestroyDescriptorPool(device, descPool, nullptr);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        if (computePipeline) vkDestroyPipeline(device, computePipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descSetLayout) vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
        if (shaderModule) vkDestroyShaderModule(device, shaderModule, nullptr);

        descPool = VK_NULL_HANDLE;
        commandPool = VK_NULL_HANDLE;
        computePipeline = VK_NULL_HANDLE;
        pipelineLayout = VK_NULL_HANDLE;
        descSetLayout = VK_NULL_HANDLE;
        shaderModule = VK_NULL_HANDLE;
        pipeline_initialized = false;
    }
};

// Gerenciamento global de instâncias, dispositivos e swapchains
static std::mutex g_mutex;
static std::unordered_map<void*, InstanceData> g_instances;
static std::unordered_map<void*, DeviceData> g_devices;
static std::unordered_map<VkPhysicalDevice, void*> g_phys_to_instance_key;
static std::unordered_map<VkSwapchainKHR, std::unique_ptr<SwapchainData>> g_swapchains;

} // namespace chroma

using namespace chroma;

// Interceptação de funções da API Vulkan
extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName);

#define HOOK_MATCH(name) if (std::strcmp(pName, #name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(name)

// Loader negotiation (Versão 2 do Vulkan Loader)
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
        pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    } else {
        pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    }
    std::cerr << "[chroma-layer] Layer ativada via Vulkan Loader negotiation." << std::endl;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkInstance *pInstance)
{
    // Localiza os dados de encadeamento de camadas (link info)
    VkLayerInstanceLink *layerLink = nullptr;
    auto *chain = reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext);
    while (chain) {
        if (chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            chain->function == VK_LAYER_LINK_INFO) {
            layerLink = chain->u.pLayerInfo;
            break;
        }
        chain = reinterpret_cast<const VkLayerInstanceCreateInfo*>(chain->pNext);
    }

    if (!layerLink) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr next_gipa = layerLink->pfnNextGetInstanceProcAddr;
    auto* mutChain = const_cast<VkLayerInstanceCreateInfo*>(chain);
    mutChain->u.pLayerInfo = layerLink->pNext;

    auto next_create_instance = reinterpret_cast<PFN_vkCreateInstance>(
        next_gipa(VK_NULL_HANDLE, "vkCreateInstance")
    );
    if (!next_create_instance) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = next_create_instance(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    std::lock_guard<std::mutex> lock(g_mutex);
    void* key = get_dispatch_key(*pInstance);
    InstanceData data{};
    data.instance = *pInstance;
    data.next_gipa = next_gipa;
    data.vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*pInstance, "vkDestroyInstance"));
    data.vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(*pInstance, "vkCreateDevice"));
    data.vkGetPhysicalDeviceFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(next_gipa(*pInstance, "vkGetPhysicalDeviceFormatProperties"));
    data.vkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(next_gipa(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties"));

    g_instances[key] = data;

    // Registra os dispositivos físicos associados a esta instância
    auto pfnEnumPhys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(next_gipa(*pInstance, "vkEnumeratePhysicalDevices"));
    if (pfnEnumPhys) {
        uint32_t count = 0;
        if (pfnEnumPhys(*pInstance, &count, nullptr) == VK_SUCCESS && count > 0) {
            std::vector<VkPhysicalDevice> gpus(count);
            if (pfnEnumPhys(*pInstance, &count, gpus.data()) == VK_SUCCESS) {
                for (auto gpu : gpus) {
                    g_phys_to_instance_key[gpu] = key;
                }
            }
        }
    }

    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks *pAllocator)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    void* key = get_dispatch_key(instance);
    auto it = g_instances.find(key);
    if (it != g_instances.end()) {
        PFN_vkDestroyInstance next_destroy = it->second.vkDestroyInstance;
        g_instances.erase(it);
        if (next_destroy) next_destroy(instance, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDevice *pDevice)
{
    VkLayerDeviceLink *layerLink = nullptr;
    auto *chain = reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext);
    while (chain) {
        if (chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            chain->function == VK_LAYER_LINK_INFO) {
            layerLink = chain->u.pLayerInfo;
            break;
        }
        chain = reinterpret_cast<const VkLayerDeviceCreateInfo*>(chain->pNext);
    }

    if (!layerLink) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr next_gipa = layerLink->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = layerLink->pfnNextGetDeviceProcAddr;
    auto* mutChain = const_cast<VkLayerDeviceCreateInfo*>(chain);
    mutChain->u.pLayerInfo = layerLink->pNext;

    PFN_vkCreateDevice next_create_device = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto instIt = g_phys_to_instance_key.find(physicalDevice);
        if (instIt != g_phys_to_instance_key.end()) {
            auto it2 = g_instances.find(instIt->second);
            if (it2 != g_instances.end()) {
                next_create_device = it2->second.vkCreateDevice;
            }
        }
    }
    if (!next_create_device) {
        next_create_device = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    }
    if (!next_create_device) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = next_create_device(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    std::lock_guard<std::mutex> lock(g_mutex);
    void* devKey = get_dispatch_key(*pDevice);
    DeviceData devData{};
    devData.device = *pDevice;
    devData.physicalDevice = physicalDevice;
    devData.next_gdpa = next_gdpa;

    auto instKeyIt = g_phys_to_instance_key.find(physicalDevice);
    if (instKeyIt != g_phys_to_instance_key.end()) {
        devData.instData = &g_instances[instKeyIt->second];
    }

    // Carrega a tabela de despacho do dispositivo
    #define LOAD_DEV_PROC(name) devData.name = reinterpret_cast<PFN_##name>(next_gdpa(*pDevice, #name))
    LOAD_DEV_PROC(vkDestroyDevice);
    LOAD_DEV_PROC(vkCreateSwapchainKHR);
    LOAD_DEV_PROC(vkDestroySwapchainKHR);
    LOAD_DEV_PROC(vkGetSwapchainImagesKHR);
    LOAD_DEV_PROC(vkQueuePresentKHR);
    LOAD_DEV_PROC(vkCreateShaderModule);
    LOAD_DEV_PROC(vkDestroyShaderModule);
    LOAD_DEV_PROC(vkCreateDescriptorSetLayout);
    LOAD_DEV_PROC(vkDestroyDescriptorSetLayout);
    LOAD_DEV_PROC(vkCreateDescriptorPool);
    LOAD_DEV_PROC(vkDestroyDescriptorPool);
    LOAD_DEV_PROC(vkAllocateDescriptorSets);
    LOAD_DEV_PROC(vkUpdateDescriptorSets);
    LOAD_DEV_PROC(vkCreatePipelineLayout);
    LOAD_DEV_PROC(vkDestroyPipelineLayout);
    LOAD_DEV_PROC(vkCreateComputePipelines);
    LOAD_DEV_PROC(vkDestroyPipeline);
    LOAD_DEV_PROC(vkCreateImageView);
    LOAD_DEV_PROC(vkDestroyImageView);
    LOAD_DEV_PROC(vkCreateCommandPool);
    LOAD_DEV_PROC(vkDestroyCommandPool);
    LOAD_DEV_PROC(vkAllocateCommandBuffers);
    LOAD_DEV_PROC(vkResetCommandBuffer);
    LOAD_DEV_PROC(vkBeginCommandBuffer);
    LOAD_DEV_PROC(vkEndCommandBuffer);
    LOAD_DEV_PROC(vkCmdPipelineBarrier);
    LOAD_DEV_PROC(vkCmdBindPipeline);
    LOAD_DEV_PROC(vkCmdBindDescriptorSets);
    LOAD_DEV_PROC(vkCmdPushConstants);
    LOAD_DEV_PROC(vkCmdDispatch);
    LOAD_DEV_PROC(vkQueueSubmit);
    LOAD_DEV_PROC(vkCreateSemaphore);
    LOAD_DEV_PROC(vkDestroySemaphore);
    LOAD_DEV_PROC(vkCreateFence);
    LOAD_DEV_PROC(vkDestroyFence);
    LOAD_DEV_PROC(vkWaitForFences);
    LOAD_DEV_PROC(vkResetFences);
    LOAD_DEV_PROC(vkDeviceWaitIdle);
    #undef LOAD_DEV_PROC

    // Identifica a fila de execução (normalmente a primeira requisitada, ex: Graphics/Compute)
    if (pCreateInfo->queueCreateInfoCount > 0) {
        devData.queueFamilyIndex = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
    }

    g_devices[devKey] = devData;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks *pAllocator)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    void* devKey = get_dispatch_key(device);
    auto it = g_devices.find(devKey);
    if (it != g_devices.end()) {
        it->second.cleanup_pipeline();
        PFN_vkDestroyDevice next_destroy = it->second.vkDestroyDevice;
        g_devices.erase(it);
        if (next_destroy) next_destroy(device, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSwapchainKHR *pSwapchain)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    void* devKey = get_dispatch_key(device);
    auto devIt = g_devices.find(devKey);
    if (devIt == g_devices.end()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    DeviceData& dev = devIt->second;

    // Requisito 3: Verifica se o formato suporta VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT
    bool supports_storage = false;
    if (dev.instData && dev.instData->vkGetPhysicalDeviceFormatProperties) {
        VkFormatProperties props{};
        dev.instData->vkGetPhysicalDeviceFormatProperties(dev.physicalDevice, pCreateInfo->imageFormat, &props);
        supports_storage = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    }

    VkSwapchainCreateInfoKHR modifiedInfo = *pCreateInfo;
    if (supports_storage) {
        modifiedInfo.imageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
    } else {
        std::cerr << "[chroma-layer] Aviso: Formato " << pCreateInfo->imageFormat
                  << " não possui VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT no hardware." << std::endl;
    }

    VkResult res = dev.vkCreateSwapchainKHR(device, &modifiedInfo, pAllocator, pSwapchain);
    // Caso o driver rejeite a adição da flag por restrição específica da swapchain, fallback
    if (res != VK_SUCCESS && supports_storage) {
        supports_storage = false;
        res = dev.vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    if (res != VK_SUCCESS) return res;

    // Se o uso de Storage Image é suportado, inicializa os recursos da camada para a swapchain
    if (supports_storage && dev.init_chroma_pipeline()) {
        auto swapData = std::make_unique<SwapchainData>();
        swapData->swapchain = *pSwapchain;
        swapData->deviceData = &dev;
        swapData->format = pCreateInfo->imageFormat;
        swapData->extent = pCreateInfo->imageExtent;
        swapData->storage_supported = true;

        uint32_t imageCount = 0;
        dev.vkGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);
        std::vector<VkImage> vkImages(imageCount);
        dev.vkGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, vkImages.data());

        swapData->images.resize(imageCount);

        for (uint32_t i = 0; i < imageCount; ++i) {
            auto& imgRes = swapData->images[i];
            imgRes.image = vkImages[i];

            // 1. ImageView como Storage Image
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = imgRes.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = pCreateInfo->imageFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            if (dev.vkCreateImageView(device, &viewInfo, nullptr, &imgRes.storageView) != VK_SUCCESS) {
                std::cerr << "[chroma-layer] Erro ao criar storage ImageView para imagem " << i << std::endl;
                continue;
            }

            // 2. Aloca e atualiza o Descriptor Set
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = dev.descPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &dev.descSetLayout;

            if (dev.vkAllocateDescriptorSets(device, &allocInfo, &imgRes.descSet) == VK_SUCCESS) {
                VkDescriptorImageInfo descImgInfo{};
                descImgInfo.imageView = imgRes.storageView;
                descImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                VkWriteDescriptorSet writeDesc{};
                writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writeDesc.dstSet = imgRes.descSet;
                writeDesc.dstBinding = 0;
                writeDesc.descriptorCount = 1;
                writeDesc.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writeDesc.pImageInfo = &descImgInfo;

                dev.vkUpdateDescriptorSets(device, 1, &writeDesc, 0, nullptr);
            }

            // 3. Command Buffer alocado do Command Pool
            VkCommandBufferAllocateInfo cmdAllocInfo{};
            cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cmdAllocInfo.commandPool = dev.commandPool;
            cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmdAllocInfo.commandBufferCount = 1;
            dev.vkAllocateCommandBuffers(device, &cmdAllocInfo, &imgRes.cmdBuffer);

            // 4. Semáforo para sinalizar após o término do Compute Shader
            VkSemaphoreCreateInfo semInfo{};
            semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            dev.vkCreateSemaphore(device, &semInfo, nullptr, &imgRes.postComputeSemaphore);

            // 5. Fence de sincronização para reaproveitamento do Command Buffer
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = 0;
            dev.vkCreateFence(device, &fenceInfo, nullptr, &imgRes.fence);
            imgRes.fence_in_flight = false;
        }

        g_swapchains[*pSwapchain] = std::move(swapData);
        std::cerr << "[chroma-layer] Swapchain interceptada e configurada para processamento via Compute Shader!" << std::endl;
    }

    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *pAllocator)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_swapchains.find(swapchain);
    if (it != g_swapchains.end()) {
        DeviceData* dev = it->second->deviceData;
        if (dev) {
            if (dev->vkDeviceWaitIdle) dev->vkDeviceWaitIdle(device);

            for (auto& imgRes : it->second->images) {
                if (imgRes.fence) dev->vkDestroyFence(device, imgRes.fence, nullptr);
                if (imgRes.postComputeSemaphore) dev->vkDestroySemaphore(device, imgRes.postComputeSemaphore, nullptr);
                if (imgRes.storageView) dev->vkDestroyImageView(device, imgRes.storageView, nullptr);
            }
        }
        g_swapchains.erase(it);
    }

    void* devKey = get_dispatch_key(device);
    auto devIt = g_devices.find(devKey);
    if (devIt != g_devices.end() && devIt->second.vkDestroySwapchainKHR) {
        devIt->second.vkDestroySwapchainKHR(device, swapchain, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR *pPresentInfo)
{
    // Requisito 3: Verifica se /tmp/chroma.conf foi modificado via stat/mtime antes de apresentar
    ConfigManager::instance().check_and_reload();

    if (!pPresentInfo || pPresentInfo->swapchainCount == 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    SwapchainData* swapData = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_swapchains.find(pPresentInfo->pSwapchains[0]);
        if (it != g_swapchains.end()) {
            swapData = it->second.get();
        }
    }

    if (!swapData || !swapData->storage_supported) {
        // Fallback direto caso a swapchain não esteja gerenciada pela camada
        void* devKey = swapData ? get_dispatch_key(swapData->deviceData->device) : nullptr;
        if (devKey) {
            std::lock_guard<std::mutex> lock(g_mutex);
            return g_devices[devKey].vkQueuePresentKHR(queue, pPresentInfo);
        }
        return VK_SUCCESS;
    }

    DeviceData* dev = swapData->deviceData;
    uint32_t imageIndex = pPresentInfo->pImageIndices[0];
    if (imageIndex >= swapData->images.size()) {
        return dev->vkQueuePresentKHR(queue, pPresentInfo);
    }

    auto& imgRes = swapData->images[imageIndex];

    // Sincronização: se a imagem já estava em execução prévia, espera o fence ser sinalizado
    if (imgRes.fence_in_flight) {
        dev->vkWaitForFences(dev->device, 1, &imgRes.fence, VK_TRUE, UINT64_MAX);
        dev->vkResetFences(dev->device, 1, &imgRes.fence);
        imgRes.fence_in_flight = false;
    }

    // Gravação do command buffer para o Compute Shader
    dev->vkResetCommandBuffer(imgRes.cmdBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev->vkBeginCommandBuffer(imgRes.cmdBuffer, &beginInfo);

    // 1. Barreira: Transição da imagem da swapchain de PRESENT_SRC_KHR para GENERAL (Storage)
    VkImageMemoryBarrier barrierToGen{};
    barrierToGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrierToGen.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrierToGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrierToGen.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrierToGen.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrierToGen.image = imgRes.image;
    barrierToGen.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrierToGen.subresourceRange.baseMipLevel = 0;
    barrierToGen.subresourceRange.levelCount = 1;
    barrierToGen.subresourceRange.baseArrayLayer = 0;
    barrierToGen.subresourceRange.layerCount = 1;
    barrierToGen.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrierToGen.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    dev->vkCmdPipelineBarrier(
        imgRes.cmdBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrierToGen
    );

    // 2. Bind do Pipeline de Compute e do Descriptor Set
    dev->vkCmdBindPipeline(imgRes.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, dev->computePipeline);
    dev->vkCmdBindDescriptorSets(
        imgRes.cmdBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        dev->pipelineLayout,
        0, 1, &imgRes.descSet,
        0, nullptr
    );

    // 3. Atualização das Push Constants (Saturação, Contraste, Brilho)
    ChromaPushConstants pc = ConfigManager::instance().get();
    dev->vkCmdPushConstants(
        imgRes.cmdBuffer,
        dev->pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(ChromaPushConstants), &pc
    );

    // 4. Despacho do Compute Shader (16x16 por workgroup)
    uint32_t groupX = (swapData->extent.width + 15) / 16;
    uint32_t groupY = (swapData->extent.height + 15) / 16;
    dev->vkCmdDispatch(imgRes.cmdBuffer, groupX, groupY, 1);

    // 5. Barreira: Transição da imagem da swapchain de volta para PRESENT_SRC_KHR
    VkImageMemoryBarrier barrierToPres = barrierToGen;
    barrierToPres.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrierToPres.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrierToPres.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrierToPres.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

    dev->vkCmdPipelineBarrier(
        imgRes.cmdBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrierToPres
    );

    dev->vkEndCommandBuffer(imgRes.cmdBuffer);

    // 6. Submissão à fila: Aguarda os semáforos originais da aplicação e sinaliza postComputeSemaphore
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    std::vector<VkPipelineStageFlags> waitStages;
    if (pPresentInfo->waitSemaphoreCount > 0) {
        waitStages.assign(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
        submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages.data();
    }

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &imgRes.cmdBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &imgRes.postComputeSemaphore;

    dev->vkQueueSubmit(queue, 1, &submitInfo, imgRes.fence);
    imgRes.fence_in_flight = true;

    // 7. Modifica pPresentInfo para que a apresentação espere o término do Compute Shader
    VkPresentInfoKHR modifiedPresent = *pPresentInfo;
    modifiedPresent.waitSemaphoreCount = 1;
    modifiedPresent.pWaitSemaphores = &imgRes.postComputeSemaphore;

    return dev->vkQueuePresentKHR(queue, &modifiedPresent);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName) return nullptr;

    HOOK_MATCH(vkGetInstanceProcAddr);
    HOOK_MATCH(vkGetDeviceProcAddr);
    HOOK_MATCH(vkNegotiateLoaderLayerInterfaceVersion);
    HOOK_MATCH(vkCreateInstance);
    HOOK_MATCH(vkDestroyInstance);
    HOOK_MATCH(vkCreateDevice);
    HOOK_MATCH(vkDestroyDevice);
    HOOK_MATCH(vkCreateSwapchainKHR);
    HOOK_MATCH(vkDestroySwapchainKHR);
    HOOK_MATCH(vkQueuePresentKHR);

    if (!instance) return nullptr;

    std::lock_guard<std::mutex> lock(g_mutex);
    void* key = get_dispatch_key(instance);
    auto it = g_instances.find(key);
    if (it != g_instances.end() && it->second.next_gipa) {
        return it->second.next_gipa(instance, pName);
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    if (!pName) return nullptr;

    HOOK_MATCH(vkGetDeviceProcAddr);
    HOOK_MATCH(vkDestroyDevice);
    HOOK_MATCH(vkCreateSwapchainKHR);
    HOOK_MATCH(vkDestroySwapchainKHR);
    HOOK_MATCH(vkQueuePresentKHR);

    if (!device) return nullptr;

    std::lock_guard<std::mutex> lock(g_mutex);
    void* key = get_dispatch_key(device);
    auto it = g_devices.find(key);
    if (it != g_devices.end() && it->second.next_gdpa) {
        return it->second.next_gdpa(device, pName);
    }
    return nullptr;
}

#undef HOOK_MATCH

} // extern "C"
