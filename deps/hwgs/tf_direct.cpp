// SPDX-License-Identifier: GPL-3.0-or-later
// D3D12 -> OpenGL shared textures. No frame pixels cross the CPU boundary.
#include "tf_hwgs.h"
#include "kzgs_internal.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include "GS/Renderers/DX12/GSTexture12.h"
#include <array>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <vector>

namespace {
enum class State { Free, Writing, Ready, Acquired };
struct Slot {
    wil::com_ptr_nothrow<ID3D12Resource> texture;
    wil::com_ptr_nothrow<ID3D12Fence> fence;
    HANDLE memoryHandle = nullptr, fenceHandle = nullptr;
    State state = State::Free;
    TfGpuFrame info{};
    uint64_t nextValue = 1, waitValue = 0;
};
std::array<Slot,3> slots;
std::mutex mutex;
uint64_t sequence = 0;
struct FrameTrace {
    struct Row {
        uint64_t hostUs, sequence, tick;
        uint32_t width, height, scene, displayFbp;
        char event;
    };
    FILE* file = nullptr;
    std::vector<Row> rows;
    bool capped = false;
    FrameTrace() {
        const char* path = std::getenv("PS2X_TF_PRESENT_TRACE");
        if (!path || !*path) return;
        if (fopen_s(&file, path, "wx") || !file)
            throw std::runtime_error("Cannot create fresh private direct-frame trace");
        rows.reserve(20000);
    }
    ~FrameTrace() { if (file) std::fclose(file); }
    // Calls are serialized by the existing presentation-slot mutex.
    void record(char event, const TfGpuFrame& frame) {
        if (!file) return;
        if (rows.size() == 200000) { capped = true; return; }
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        rows.push_back({static_cast<uint64_t>(us), frame.sequence, frame.tick, frame.width, frame.height,
            frame.scene, frame.display_fbp, event});
    }
    void close() {
        if (!file) return;
        std::fputs("event,host_us,sequence,tick,width,height,scene,display_fbp\n", file);
        bool failed = false;
        for (const auto& row : rows) {
            failed |= std::fprintf(file, "%c,%llu,%llu,%llu,%u,%u,%u,%u\n", row.event,
                static_cast<unsigned long long>(row.hostUs), static_cast<unsigned long long>(row.sequence),
                static_cast<unsigned long long>(row.tick), row.width, row.height, row.scene, row.displayFbp) < 0;
        }
        failed |= std::fclose(file) != 0;
        file = nullptr;
        std::fprintf(stderr, "[hwgs:frame-trace] rows=%zu capped=%u write_error=%u\n", rows.size(), capped, failed);
        rows.clear();
        if (failed) throw std::runtime_error("Cannot write private direct-frame trace");
    }
};
FrameTrace& frameTrace() { static FrameTrace trace; return trace; }
void check(HRESULT hr, const char* action) {
    if (FAILED(hr)) {
        char message[192]; std::snprintf(message,sizeof(message),"%s failed: %08lx",action,static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}
void waitFence(ID3D12Fence* fence, uint64_t value) {
    if (!value || fence->GetCompletedValue() >= value) return;
    HANDLE event = CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!event) throw std::runtime_error("shared texture fence event allocation failed");
    const HRESULT hr = fence->SetEventOnCompletion(value,event);
    const DWORD result = SUCCEEDED(hr) ? WaitForSingleObject(event,10000) : WAIT_FAILED;
    CloseHandle(event);
    check(hr,"SetEventOnCompletion");
    if (result != WAIT_OBJECT_0) throw std::runtime_error("shared texture resize fence timed out");
}
void destroy(Slot& s) {
    if(s.memoryHandle)CloseHandle(s.memoryHandle);
    if(s.fenceHandle)CloseHandle(s.fenceHandle);
    s.memoryHandle = s.fenceHandle = nullptr;
    s.texture.reset(); s.fence.reset(); s.waitValue = 0; s.nextValue = 1;
}
void allocate(Slot& s, GSDevice12* device, uint32_t width, uint32_t height) {
    if(s.texture && s.info.texture_width >= width && s.info.texture_height >= height) return;
    if(s.fence) {
        // Only size changes may wait on the CPU. Ordinary reuse waits on the GPU.
        waitFence(s.fence.get(),std::max(s.waitValue,s.info.ready_value));
        destroy(s);
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    check(device->GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(s.texture.put())),"CreateCommittedResource(shared frame)");
    check(device->GetDevice()->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(s.fence.put())),"CreateFence(shared frame)");
    check(device->GetDevice()->CreateSharedHandle(s.texture.get(),nullptr,GENERIC_ALL,nullptr,&s.memoryHandle),"CreateSharedHandle(texture)");
    check(device->GetDevice()->CreateSharedHandle(s.fence.get(),nullptr,GENERIC_ALL,nullptr,&s.fenceHandle),"CreateSharedHandle(fence)");
    ++s.info.generation;
    s.info.texture_width = width; s.info.texture_height = height;
    s.info.memory_handle = s.memoryHandle; s.info.fence_handle = s.fenceHandle;
    const LUID luid = device->GetDevice()->GetAdapterLuid();
    std::memcpy(s.info.adapter_luid,&luid,sizeof(luid));
    std::fprintf(stderr,"[hwgs:direct] allocated slot=%u generation=%u texture=%ux%u\n",s.info.slot,s.info.generation,width,height);
}
}

int tf_gs_direct_enabled() {
    static const bool enabled = [] { const char* p=std::getenv("PS2X_HWGS_DIRECT");return p && *p=='1'; }();
    return enabled ? 1 : 0;
}
int tf_gs_direct_publish(uint32_t scale, uint32_t scene, uint32_t display_fbp, uint64_t tick) {
    bool ok = true;
    kzgs::RunOnGSThread([&] {
        try {
            auto* device = GSDevice12::GetInstance();
            auto* current = device ? static_cast<GSTexture12*>(device->GetCurrent()) : nullptr;
            if (!current) return;
            if (current->GetDXGIFormat()!=DXGI_FORMAT_R8G8B8A8_UNORM)
                throw std::runtime_error("direct presentation requires an RGBA8 output texture");
            Slot* selected = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto& s:slots) if(s.state==State::Free) {selected=&s;break;}
                // A queued frame never acquired by GL can be replaced in D3D queue order.
                if(!selected) for(auto& s:slots) if(s.state==State::Ready && (!selected || s.info.sequence<selected->info.sequence))selected=&s;
                if(!selected)return; // All slots are being read; never overwrite one.
                if(selected->state==State::Ready)selected->waitValue=0;
                selected->state=State::Writing;
            }
            Slot& s=*selected;
            s.info.slot=static_cast<uint32_t>(&s-slots.data());
            const uint32_t width=current->GetWidth(),height=current->GetHeight();
            // Capacity covers normal alternating field dimensions without reallocating.
            allocate(s,device,std::max(width,640u*scale),std::max(height,512u*scale));
            if(s.waitValue)check(device->GetCommandQueue()->Wait(s.fence.get(),s.waitValue),"GPU wait for GL release");
            device->EndRenderPass();
            current->CommitClear();
            const auto oldState=current->GetResourceState();
            current->TransitionToState(GSTexture12::ResourceState::CopySrc);
            current->SetUseFenceCounter(device->GetCurrentFenceValue());
            auto* command=device->GetCommandList().list4.get();
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource=s.texture.get();
            barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
            command->ResourceBarrier(1,&barrier);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};
            src.pResource=current->GetResource();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource=s.texture.get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{0,0,0,width,height,1};
            command->CopyTextureRegion(&dst,0,0,0,&src,&box);
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
            command->ResourceBarrier(1,&barrier);
            current->TransitionToState(oldState);
            const uint64_t ready=s.nextValue; s.nextValue+=2;
            if(!device->ExecuteCommandList(GSDevice12::WaitType::None))throw std::runtime_error("direct frame submission failed");
            device->InvalidateCachedState();
            check(device->GetCommandQueue()->Signal(s.fence.get(),ready),"GPU signal for GL acquire");
            {
                std::lock_guard<std::mutex> lock(mutex);
                s.info.width=width;s.info.height=height;s.info.scale=scale;s.info.scene=scene;
                s.info.display_fbp=display_fbp;
                s.info.sequence=++sequence;s.info.tick=tick;s.info.ready_value=ready;s.info.release_value=ready+1;
                s.state=State::Ready;
                frameTrace().record('p', s.info);
            }
            if(sequence%300==0)std::fprintf(stderr,"[hwgs:direct] submitted=%llu output=%ux%u full_frame_cpu_bytes=0\n",static_cast<unsigned long long>(sequence),width,height);
        } catch(const std::exception& e) {std::fprintf(stderr,"[hwgs:direct] %s\n",e.what());ok=false;}
    });
    return ok ? 1 : 0;
}
int tf_gs_direct_acquire(TfGpuFrame* frame) {
    if(!frame)return 0;
    std::lock_guard<std::mutex> lock(mutex);
    Slot* latest=nullptr;
    for(auto& s:slots)if(s.state==State::Ready && (!latest || s.info.sequence>latest->info.sequence))latest=&s;
    if(!latest)return 0;
    // Retire skipped frames now. Otherwise a later host refresh could acquire an
    // older slot while the producer is still preparing the next frame.
    for(auto& s:slots)if(&s!=latest && s.state==State::Ready) {
        s.waitValue=0;s.state=State::Free;
    }
    latest->state=State::Acquired;*frame=latest->info;
    frameTrace().record('a', latest->info);
    return 1;
}
void tf_gs_direct_release(uint32_t slot,uint32_t generation) {
    std::lock_guard<std::mutex> lock(mutex);
    if(slot>=slots.size())return;
    auto& s=slots[slot];
    if(s.state==State::Acquired && s.info.generation==generation) {
        frameTrace().record('r', s.info);
        s.waitValue=s.info.release_value;s.state=State::Free;
    }
}
void tf_gs_direct_close() {
    // Caller has finished/deleted GL imports and stopped the producer first.
    kzgs::RunOnGSThread([] {
        if(g_gs_device)GSDevice12::GetInstance()->WaitForGPUIdle();
        std::lock_guard<std::mutex> lock(mutex);
        for(auto& s:slots){destroy(s);s.state=State::Free;}
        frameTrace().close();
    });
}
