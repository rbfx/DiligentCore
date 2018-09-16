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

#include "pch.h"
#include "D3D12DynamicHeap.h"
#include "RenderDeviceD3D12Impl.h"

namespace Diligent
{

D3D12DynamicPage::D3D12DynamicPage(ID3D12Device* pd3d12Device, Uint64 Size)
{
	D3D12_HEAP_PROPERTIES HeapProps;
	HeapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	HeapProps.CreationNodeMask     = 1;
	HeapProps.VisibleNodeMask      = 1;

	D3D12_RESOURCE_DESC ResourceDesc;
	ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	ResourceDesc.Alignment = 0;
	ResourceDesc.Height = 1;
	ResourceDesc.DepthOrArraySize = 1;
	ResourceDesc.MipLevels = 1;
	ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	ResourceDesc.SampleDesc.Count = 1;
	ResourceDesc.SampleDesc.Quality = 0;
	ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12_RESOURCE_STATES DefaultUsage = D3D12_RESOURCE_STATE_GENERIC_READ;
	HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
	ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	DefaultUsage = D3D12_RESOURCE_STATE_GENERIC_READ;
    ResourceDesc.Width = Size;

	auto hr = pd3d12Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
		DefaultUsage, nullptr, __uuidof(m_pd3d12Buffer), reinterpret_cast<void**>(static_cast<ID3D12Resource**>(&m_pd3d12Buffer)) );
    if(FAILED(hr))
    {
        LOG_D3D_ERROR(hr, "Failed to create dynamic page");
        return;
    }

	m_pd3d12Buffer->SetName(L"Dynamic memory page");
        
    m_GPUVirtualAddress = m_pd3d12Buffer->GetGPUVirtualAddress();
        
	m_pd3d12Buffer->Map(0, nullptr, &m_CPUVirtualAddress);

    LOG_INFO_MESSAGE("Created dynamic memory page. Size: ", FormatMemorySize(Size,2), "; GPU virtual address 0x", std::hex, m_GPUVirtualAddress);
}

D3D12DynamicMemoryManager::D3D12DynamicMemoryManager(IMemoryAllocator&  Allocator, 
                                                     ID3D12Device*      pd3d12Device,
                                                     Uint32             NumPagesToReserve,
                                                     Uint64             PageSize) : 
    m_pd3d12Device(pd3d12Device),
    m_AvailablePages(STD_ALLOCATOR_RAW_MEM(AvailablePagesMapElemType, Allocator, "Allocator for multimap<AvailablePagesMapElemType>")),
    m_StalePages(STD_ALLOCATOR_RAW_MEM(StalePageInfo, Allocator, "Allocator for deque<StalePageInfo>"))
{
    for(Uint32 i=0; i < NumPagesToReserve; ++i)
    {
        D3D12DynamicPage Page(m_pd3d12Device, PageSize);
        auto Size = Page.GetSize();
        m_AvailablePages.emplace(Size, std::move(Page));
    }
}

D3D12DynamicPage D3D12DynamicMemoryManager::AllocatePage(Uint64 SizeInBytes)
{
    std::lock_guard<std::mutex> AvailablePagesLock(m_AvailablePagesMtx);
    auto PageIt = m_AvailablePages.lower_bound(SizeInBytes); // Returns an iterator pointing to the first element that is not less than key
    if (PageIt != m_AvailablePages.end())
    {
        VERIFY_EXPR(PageIt->first >= SizeInBytes);
        D3D12DynamicPage Page(std::move(PageIt->second));
        m_AvailablePages.erase(PageIt);
        return Page;
    }
    else
    {
        return D3D12DynamicPage{m_pd3d12Device, SizeInBytes};
    }
}

void D3D12DynamicMemoryManager::Destroy(Uint64 LastCompletedFenceValue)
{
    ReleaseStalePages(LastCompletedFenceValue);
    DEV_CHECK_ERR(m_StalePages.empty(), "Not all stale pages have been released and are still in use. The device must be idled before calling Destroy()");
    Uint64 TotalAllocatedSize = 0;
    for(const auto& Page : m_AvailablePages)
        TotalAllocatedSize += Page.second.GetSize();

    LOG_INFO_MESSAGE("Dynamic memory manager usage stats:\n"
                     "                       Total allocated memory: ", FormatMemorySize(TotalAllocatedSize, 2));

    m_StalePages.clear();
    m_AvailablePages.clear();
}

D3D12DynamicMemoryManager::~D3D12DynamicMemoryManager()
{
    VERIFY(m_AvailablePages.empty() && m_StalePages.empty(), "Not all pages are destroyed. Dynamic memory manager must be explicitly destroyed with Destroy() method");
}


D3D12DynamicHeap::~D3D12DynamicHeap()
{
    VERIFY(m_AllocatedPages.empty(), "Allocated pages have not been released which indicates FinishFrame() has not been called");
    
    LOG_INFO_MESSAGE(m_HeapName, " usage stats:\n"
        "                       Peak used/peak allocated size: ", FormatMemorySize(m_PeakUsedSize, 2, m_PeakAllocatedSize), '/', FormatMemorySize(m_PeakAllocatedSize, 2, m_PeakAllocatedSize),
        ". Peak utilization: ", std::fixed, std::setprecision(1), static_cast<double>(m_PeakUsedSize) / static_cast<double>(std::max(m_PeakAllocatedSize, Uint64{1})) * 100.0, '%');
}

D3D12DynamicAllocation D3D12DynamicHeap::Allocate(Uint64 SizeInBytes, Uint64 Alignment, Uint64 DvpCtxFrameNumber)
{
    VERIFY_EXPR(Alignment > 0);
    VERIFY(IsPowerOfTwo(Alignment), "Alignment (", Alignment, ") must be power of 2");

    if (m_CurrOffset == InvalidOffset || SizeInBytes + (Align(m_CurrOffset, Alignment) - m_CurrOffset) > m_AvailableSize)
    {
        auto NewPageSize = m_PageSize;
        while(NewPageSize < SizeInBytes)
            NewPageSize *= 2;

        auto NewPage = m_DynamicMemMgr.AllocatePage(NewPageSize);
        if (NewPage.IsValid())
        {
            m_CurrOffset    = 0;
            m_AvailableSize = NewPage.GetSize();
            
            m_CurrAllocatedSize += m_AvailableSize;
            m_PeakAllocatedSize  = std::max(m_PeakAllocatedSize, m_CurrAllocatedSize);

            m_AllocatedPages.emplace_back(std::move(NewPage));
        }
    }

    if (m_CurrOffset != InvalidOffset && SizeInBytes + (Align(m_CurrOffset, Alignment) - m_CurrOffset) <= m_AvailableSize)
    {
        auto AlignedOffset = Align(m_CurrOffset, Alignment);
        auto AdjustedSize = SizeInBytes + (AlignedOffset - m_CurrOffset);
        VERIFY_EXPR(AdjustedSize <= m_AvailableSize);
        m_AvailableSize -= AdjustedSize;
        m_CurrOffset    += AdjustedSize;
        
        m_CurrUsedSize += SizeInBytes;
        m_PeakUsedSize  = std::max(m_PeakUsedSize,      m_CurrUsedSize);
        
        auto& CurrPage = m_AllocatedPages.back();
        return D3D12DynamicAllocation
        { 
            CurrPage.GetD3D12Buffer(),
            AlignedOffset,
            SizeInBytes,
            CurrPage.GetCPUAddress(AlignedOffset),
            CurrPage.GetGPUAddress(AlignedOffset)
#ifdef DEVELOPMENT
          , DvpCtxFrameNumber
#endif
        };
    }
    else
        return D3D12DynamicAllocation{};
}

void D3D12DynamicHeap::FinishFrame(Uint64 FenceValue)
{
    m_DynamicMemMgr.DiscardPages(m_AllocatedPages, FenceValue);
    m_AllocatedPages.clear();

    m_CurrOffset        = InvalidOffset;
    m_AvailableSize     = 0;
    m_CurrAllocatedSize = 0;
    m_CurrUsedSize      = 0;
}

}
