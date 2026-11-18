#pragma once

#include <vulkan/vulkan.h>
#include <cstring>
#include <cstdint>

#if __has_include(<vulkan/vk_layer.h>)
#include <vulkan/vk_layer.h>
#else
// Definições padrão da especificação Khronos Vulkan Loader caso vk_layer.h não esteja no sistema
#define LAYER_NEGOTIATE_INTERFACE_STRUCT 1

typedef enum VkNegotiateLayerStructType {
    LAYER_NEGOTIATE_UNINITIALIZED = 0,
    LAYER_NEGOTIATE_INTERFACE_STRUCT = 1,
} VkNegotiateLayerStructType;

typedef VkResult (VKAPI_PTR *PFN_GetPhysicalDeviceProcAddr)(VkInstance instance, const char *pName);

typedef struct VkNegotiateLayerInterface {
    VkNegotiateLayerStructType sType;
    void *pNext;
    uint32_t loaderLayerInterfaceVersion;
    PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr;
    PFN_GetPhysicalDeviceProcAddr pfnGetPhysicalDeviceProcAddr;
} VkNegotiateLayerInterface;

#define VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO static_cast<VkStructureType>(47)
#define VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO   static_cast<VkStructureType>(48)

typedef enum VkLayerFunction_ {
    VK_LAYER_LINK_INFO = 0,
    VK_LOADER_DATA_CALLBACK = 1
} VkLayerFunction;

typedef struct VkLayerInstanceLink_ {
    struct VkLayerInstanceLink_ *pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_GetPhysicalDeviceProcAddr pfnNextGetPhysicalDeviceProcAddr;
} VkLayerInstanceLink;

typedef struct VkLayerInstanceCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkLayerFunction function;
    union {
        VkLayerInstanceLink *pLayerInfo;
        void *pfnSetInstanceLoaderData;
    } u;
} VkLayerInstanceCreateInfo;

typedef struct VkLayerDeviceLink_ {
    struct VkLayerDeviceLink_ *pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
} VkLayerDeviceLink;

typedef struct VkLayerDeviceCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkLayerFunction function;
    union {
        VkLayerDeviceLink *pLayerInfo;
        void *pfnSetDeviceLoaderData;
    } u;
} VkLayerDeviceCreateInfo;
#endif

// Retorna a chave de despacho do loader Vulkan para um objeto despachável
inline void* get_dispatch_key(const void* handle) {
    return handle ? *reinterpret_cast<void* const*>(handle) : nullptr;
}
