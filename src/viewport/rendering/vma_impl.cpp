// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file vma_impl.cpp
 * @brief The one translation unit that compiles AMD's Vulkan Memory Allocator.
 *
 * VMA is header-only: exactly one .cpp in the whole project must define
 * VMA_IMPLEMENTATION before including the header. Everyone else just includes
 * <vk_mem_alloc.h> for the declarations. Keep this file otherwise empty.
 *
 * We bind VMA to the statically-linked Vulkan loader (CMake's Vulkan::Vulkan target)
 * rather than handing it a function-pointer table, which is why VulkanContext can call
 * vmaCreateAllocator() without populating VmaVulkanFunctions.
 */

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

#include <vk_mem_alloc.h>
