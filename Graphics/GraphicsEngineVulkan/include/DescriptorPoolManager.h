/*     Copyright 2015-2018 Egor Yusov
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF ANY PROPRIETARY RIGHTS.
 *
 *  In no event and under no legal theory, whether in tort (including negligence), 
 *  contract, or otherwise, unless required by applicable law (such as deliberate 
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental, 
 *  or consequential damages of any character arising as a result of this License or 
 *  out of the use or inability to use the software (including but not limited to damages 
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and 
 *  all other commercial damages or losses), even if such Contributor has been advised 
 *  of the possibility of such damages.
 */

// Descriptor heap management utilities. 
// See http://diligentgraphics.com/diligent-engine/architecture/d3d12/managing-descriptor-heaps/ for details

#pragma once

#include <vector>
#include <deque>
#include <mutex>
#include "VulkanUtilities/VulkanObjectWrappers.h"

namespace Diligent
{

class DescriptorSetAllocator;
class RenderDeviceVkImpl;

// This class manages descriptor set allocation. 
// The class destructor calls DescriptorSetAllocator::FreeDescriptorSet() that moves
// the set into the release queue.
class DescriptorSetAllocation
{
public:
    DescriptorSetAllocation(VkDescriptorSet         _Set,
                            VkDescriptorPool        _Pool,
                            Uint64                  _CmdQueueMask,
                            DescriptorSetAllocator& _DescrSetAllocator)noexcept :
        Set              (_Set),
        Pool             (_Pool),
        CmdQueueMask     (_CmdQueueMask),
        DescrSetAllocator(&_DescrSetAllocator)
    {}
    DescriptorSetAllocation()noexcept{}

    DescriptorSetAllocation             (const DescriptorSetAllocation&) = delete;
    DescriptorSetAllocation& operator = (const DescriptorSetAllocation&) = delete;

    DescriptorSetAllocation(DescriptorSetAllocation&& rhs)noexcept : 
        Set              (rhs.Set),
        CmdQueueMask     (rhs.CmdQueueMask),
        Pool             (rhs.Pool),
        DescrSetAllocator(rhs.DescrSetAllocator)
    {
        rhs.Reset();
    }
    
    DescriptorSetAllocation& operator = (DescriptorSetAllocation&& rhs)noexcept
    {
        Release();

        Set               = rhs.Set;
        CmdQueueMask      = rhs.CmdQueueMask;
        Pool              = rhs.Pool;
        DescrSetAllocator = rhs.DescrSetAllocator;

        rhs.Reset();

        return *this;
    }

    operator bool()const
    {
        return Set != VK_NULL_HANDLE;
    }

    void Reset()
    {
        Set               = VK_NULL_HANDLE;
        Pool              = VK_NULL_HANDLE;
        CmdQueueMask      = 0;
        DescrSetAllocator = nullptr;
    }

    void Release();

    ~DescriptorSetAllocation()
    {
        Release();
    }
    
    VkDescriptorSet GetVkDescriptorSet()const {return Set;}

private:
    VkDescriptorSet         Set               = VK_NULL_HANDLE;
    VkDescriptorPool        Pool              = VK_NULL_HANDLE;
    Uint64                  CmdQueueMask      = 0;
    DescriptorSetAllocator* DescrSetAllocator = nullptr;
};

// The class manages pool of descriptor set pools
class DescriptorPoolManager
{
public:
    DescriptorPoolManager(RenderDeviceVkImpl&               DeviceVkImpl,
                          std::string                       PoolName,
                          std::vector<VkDescriptorPoolSize> PoolSizes,
                          uint32_t                          MaxSets,
                          bool                              AllowFreeing) noexcept:
        m_DeviceVkImpl(DeviceVkImpl),
        m_PoolName    (std::move(PoolName)),
        m_PoolSizes   (std::move(PoolSizes)),
        m_MaxSets     (MaxSets),
        m_AllowFreeing(AllowFreeing)
    {
    }
    ~DescriptorPoolManager();

    DescriptorPoolManager             (const DescriptorPoolManager&) = delete;
    DescriptorPoolManager& operator = (const DescriptorPoolManager&) = delete;
    DescriptorPoolManager             (DescriptorPoolManager&&)      = delete;
    DescriptorPoolManager& operator = (DescriptorPoolManager&&)      = delete;
    
    VulkanUtilities::DescriptorPoolWrapper GetPool(const char* DebugName);
    void FreePool(VulkanUtilities::DescriptorPoolWrapper&& Pool);

protected:
    friend class DynamicDescriptorSetAllocator;
    VulkanUtilities::DescriptorPoolWrapper CreateDescriptorPool(const char* DebugName);

    RenderDeviceVkImpl& m_DeviceVkImpl;
    const std::string   m_PoolName;

    const std::vector<VkDescriptorPoolSize> m_PoolSizes;
    const uint32_t                          m_MaxSets;
    const bool                              m_AllowFreeing;
    
    std::mutex                                           m_Mutex;
    std::deque< VulkanUtilities::DescriptorPoolWrapper > m_Pools;
};

// The class allocates descriptors from the main descriptor pool.
// Descriptors can be released and returned to the pool
class DescriptorSetAllocator : public DescriptorPoolManager
{
public:
    friend class DescriptorSetAllocation;
    DescriptorSetAllocator(RenderDeviceVkImpl&               DeviceVkImpl,
                           std::string                       PoolName,
                           std::vector<VkDescriptorPoolSize> PoolSizes,
                           uint32_t                          MaxSets,
                           bool                              AllowFreeing) noexcept:
        DescriptorPoolManager(DeviceVkImpl, std::move(PoolName), std::move(PoolSizes), MaxSets, AllowFreeing)
    {
    }

    DescriptorSetAllocation Allocate(Uint64 CommandQueueMask, VkDescriptorSetLayout SetLayout);

private:  
    void FreeDescriptorSet(VkDescriptorSet Set, VkDescriptorPool Pool, Uint64 QueueMask);
};

// The class manages dynamic descriptor sets. It first requests descriptor pool from
// the global manager and performs allocations from this pool. When space in the pool is exhausted,
// the class requests new pool.
// The class is not thread-safe as device contexts must not be used in multiple threads at the same time.
// Entire pools are recycled at the end of every frame.
class DynamicDescriptorSetAllocator
{
public:
    DynamicDescriptorSetAllocator(DescriptorPoolManager& PoolMgr, std::string Name) : 
        m_PoolMgr(PoolMgr),
        m_Name(std::move(Name))
    {}
    ~DynamicDescriptorSetAllocator();

    VkDescriptorSet Allocate(VkDescriptorSetLayout SetLayout, const char* DebugName);
    void ReleasePools(Uint64 QueueMask);
    size_t GetAllocatedPoolCount()const{return m_AllocatedPools.size();}

private:
    DescriptorPoolManager&                              m_PoolMgr;
    const std::string                                   m_Name;
    std::vector<VulkanUtilities::DescriptorPoolWrapper> m_AllocatedPools;
    size_t                                              m_PeakPoolCount = 0;
};

}
