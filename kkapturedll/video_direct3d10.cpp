/* kkapture: intrusive demo video capturing.
 * Copyright (c) 2005-2010 Fabian "ryg/farbrausch" Giesen.
 *
 * This program is free software; you can redistribute and/or modify it under
 * the terms of the Artistic License, Version 2.0beta5, or (at your opinion)
 * any later version; all distributions of this program should contain this
 * license in a file named "LICENSE.txt".
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT UNLESS REQUIRED BY
 * LAW OR AGREED TO IN WRITING WILL ANY COPYRIGHT HOLDER OR CONTRIBUTOR
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "stdafx.h"
#include "video.h"
#include "videoencoder.h"

#include <InitGuid.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3d11.h>
#include <d3d10.h>

static HRESULT(__stdcall* Real_CreateDXGIFactory)(REFIID riid, void** ppFactory) = 0;
static HRESULT(__stdcall* Real_CreateDXGIFactory2)(UINT flag, REFIID riid, void** ppFactory) = 0;
static HRESULT(__stdcall* Real_Factory_CreateSwapChain)(IUnknown* me, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain) = 0;
static HRESULT(__stdcall* Real_SwapChain_Present)(IDXGISwapChain* me, UINT SyncInterval, UINT Flags) = 0;

static bool grabFrameD3D10(IDXGISwapChain* swap)
{
    ID3D10Device* device = 0;
    ID3D10Texture2D* tex = 0, * captureTex = 0;

    if (FAILED(swap->GetBuffer(0, IID_ID3D10Texture2D, (void**)&tex)))
        return false;

    D3D10_TEXTURE2D_DESC desc;
    tex->GetDevice(&device);
    tex->GetDesc(&desc);

    // re-creating the capture staging texture each frame is definitely not the most efficient
    // way to handle things, but it frees me of all kind of resource management trouble, so
    // here goes...
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D10_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    if (FAILED(device->CreateTexture2D(&desc, 0, &captureTex)))
        printLog("video/d3d10: couldn't create staging texture for gpu->cpu download!\n");
    else
        setCaptureResolution(desc.Width, desc.Height);

    if (device)
        device->CopySubresourceRegion(captureTex, 0, 0, 0, 0, tex, 0, 0);

    D3D10_MAPPED_TEXTURE2D mapped;
    bool grabOk = false;

    if (captureTex && SUCCEEDED(captureTex->Map(0, D3D10_MAP_READ, 0, &mapped)))
    {
        switch (desc.Format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            blitAndFlipRGBAToCaptureData((unsigned char*)mapped.pData, mapped.RowPitch);
            grabOk = true;
            break;

        default:
            printLog("video/d3d10: unsupported backbuffer format, can't grab pixels!\n");
            break;
        }

        captureTex->Unmap(0);
    }

    tex->Release();
    if (captureTex) captureTex->Release();
    if (device) device->Release();

    return grabOk;
}

static bool grabFrameD3D11(IDXGISwapChain* swap)
{
    ID3D11Device* device = 0;
    ID3D11DeviceContext* context = 0;
    ID3D11Texture2D* tex = 0, * captureTex = 0;

    if (FAILED(swap->GetBuffer(0, IID_ID3D11Texture2D, (void**)&tex)))
        return false;

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDevice(&device);
    tex->GetDesc(&desc);

    // re-creating the capture staging texture each frame is definitely not the most efficient
    // way to handle things, but it frees me of all kind of resource management trouble, so
    // here goes...
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    if (FAILED(device->CreateTexture2D(&desc, 0, &captureTex)))
        printLog("video/d3d11: couldn't create staging texture for gpu->cpu download!\n");
    else
        setCaptureResolution(desc.Width, desc.Height);

    device->GetImmediateContext(&context);
    context->CopySubresourceRegion(captureTex, 0, 0, 0, 0, tex, 0, 0);

    D3D11_MAPPED_SUBRESOURCE mapped;
    bool grabOk = false;

    if (captureTex && SUCCEEDED(context->Map(captureTex, 0, D3D11_MAP_READ, 0, &mapped)))
    {
        switch (desc.Format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            blitAndFlipRGBAToCaptureData((unsigned char*)mapped.pData, mapped.RowPitch);
            grabOk = true;
            break;

        default:
            printLog("video/d3d11: unsupported backbuffer format, can't grab pixels!\n");
            break;
        }

        context->Unmap(captureTex, 0);
    }

    tex->Release();
    if (captureTex) captureTex->Release();
    context->Release();
    device->Release();

    return grabOk;
}

static bool grabFrameD3D12(IDXGISwapChain* swap)
{
    ID3D12Device* device = 0;
    ID3D12Resource* frameResource = 0;
    if (SUCCEEDED(swap->GetBuffer(0, IID_ID3D12Resource, (void**)&frameResource)) &&
        SUCCEEDED(frameResource->GetDevice(IID_ID3D12Device, (void**)&device)))
    {
        static ID3D12CommandQueue* commandQueue = 0;
        static ID3D12CommandAllocator* commandAllocator = 0;
        static ID3D12Resource* readBackResource = 0;
        static ID3D12GraphicsCommandList* commandList = 0;
        static ID3D12Fence* fence = 0;
        static HANDLE fenceEvent = 0;
        static unsigned long long fenceValue = 0;

        D3D12_COMMAND_LIST_TYPE commandListType = D3D12_COMMAND_LIST_TYPE_COPY;
        D3D12_RESOURCE_DESC frameDesc = frameResource->GetDesc();
        if (frameDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            printLog("video/d3d12: unsupported backbuffer format, can't grab pixels!\n");
            return false;
        }

        setCaptureResolution((int)frameDesc.Width, (int)frameDesc.Height);

        if (!fence)
        {
            device->CreateFence(fenceValue, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&fence);
            fenceEvent = CreateEvent(0, 0, 0, 0);
        }

        if (fence && !commandQueue)
        {
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type = commandListType;
            queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            queueDesc.NodeMask = 0;
            device->CreateCommandQueue(&queueDesc, IID_ID3D12CommandQueue, (void**)&commandQueue);
        }

        if (commandQueue && !commandAllocator)
        {
            device->CreateCommandAllocator(commandListType, IID_ID3D12CommandAllocator, (void**)&commandAllocator);
        }

        if (commandAllocator && !commandList)
        {
            if (SUCCEEDED(device->CreateCommandList(0, commandListType, commandAllocator, nullptr, IID_ID3D12GraphicsCommandList, (void**)&commandList)))
            {
                commandList->Close();
            }
        }

        if (commandList && !readBackResource)
        {
            unsigned long long BufferSize = frameDesc.Width * frameDesc.Height * sizeof(int);

            D3D12_HEAP_PROPERTIES readBackHeap;
            readBackHeap.Type = D3D12_HEAP_TYPE_READBACK;
            readBackHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            readBackHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            readBackHeap.CreationNodeMask = 0;
            readBackHeap.VisibleNodeMask = 0;

            D3D12_RESOURCE_DESC resourceDesc;
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resourceDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            resourceDesc.Width = ((BufferSize)+((256L)-1L)) & ~((256L)-1L);
            resourceDesc.Height = 1;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.SampleDesc.Quality = 0;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

            device->CreateCommittedResource(
                &readBackHeap,
                D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES,
                &resourceDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                0,
                IID_ID3D12Resource,
                (void**)&readBackResource
            );
        }

        if (readBackResource)
        {
            D3D12_TEXTURE_COPY_LOCATION dst;
            dst.pResource = readBackResource;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = 0;
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dst.PlacedFootprint.Footprint.Width = (unsigned int)frameDesc.Width;
            dst.PlacedFootprint.Footprint.Height = (unsigned int)frameDesc.Height;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = (unsigned int)frameDesc.Width * sizeof(int);

            D3D12_TEXTURE_COPY_LOCATION src;
            src.pResource = frameResource;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
        
            commandAllocator->Reset();
            commandList->Reset(commandAllocator, nullptr);
            commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, 0);
            commandList->Close();
            commandQueue->ExecuteCommandLists(1, (ID3D12CommandList**)&commandList);
            commandQueue->Signal(fence, ++fenceValue);

            if (fence->GetCompletedValue() != fenceValue)
            {
                fence->SetEventOnCompletion(fenceValue, fenceEvent);
                WaitForSingleObject(fenceEvent, INFINITE);
            }

            void* mappedResource = 0;
            if (SUCCEEDED(readBackResource->Map(0, 0, &mappedResource)))
            {
                blitAndFlipRGBAToCaptureData((unsigned char*)mappedResource, (unsigned int)frameDesc.Width * sizeof(int));
                readBackResource->Unmap(0, 0);
            }
            return true;
        }
    }
    return false;
}

static HRESULT __stdcall Mine_SwapChain_Present(IDXGISwapChain* me, UINT SyncInterval, UINT Flags)
{

    if (params.CaptureVideo)
    {
        if (grabFrameD3D11(me) || grabFrameD3D10(me) || grabFrameD3D12(me))
            encoder->WriteFrame(captureData);
    }

    HRESULT hr = Real_SwapChain_Present(me, 0, Flags);

    nextFrame();
    return hr;
}

static HRESULT __stdcall Mine_Factory_CreateSwapChain(IUnknown* me, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain)
{
    HRESULT hr = Real_Factory_CreateSwapChain(me, dev, desc, chain);
    if (SUCCEEDED(hr))
    {
        printLog("video/d3d10: swap chain created.\n");
        HookCOMOnce(&Real_SwapChain_Present, *chain, 8, Mine_SwapChain_Present);
    }

    return hr;
}

static HRESULT __stdcall Mine_CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    HRESULT hr = Real_CreateDXGIFactory(riid, ppFactory);
    if (!Real_Factory_CreateSwapChain && SUCCEEDED(hr) &&
        (riid == IID_IDXGIFactory || riid == IID_IDXGIFactory1 || riid == IID_IDXGIFactory2 || riid == IID_IDXGIFactory3 || riid == IID_IDXGIFactory4))
        HookCOMOnce(&Real_Factory_CreateSwapChain, (IUnknown*)*ppFactory, 10, Mine_Factory_CreateSwapChain);

    return hr;
}

static HRESULT __stdcall Mine_CreateDXGIFactory2(UINT flag, REFIID riid, void** ppFactory)
{
    HRESULT hr = Real_CreateDXGIFactory2(flag, riid, ppFactory);
    if (!Real_Factory_CreateSwapChain && SUCCEEDED(hr) &&
        (riid == IID_IDXGIFactory || riid == IID_IDXGIFactory1 || riid == IID_IDXGIFactory2 || riid == IID_IDXGIFactory3 || riid == IID_IDXGIFactory4))
        HookCOMOnce(&Real_Factory_CreateSwapChain, (IUnknown*)*ppFactory, 10, Mine_Factory_CreateSwapChain);

    return hr;
}

void initVideo_Direct3D10()
{
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    if (dxgi)
    {
        HookDLLFunction(&Real_CreateDXGIFactory, dxgi, "CreateDXGIFactory", Mine_CreateDXGIFactory);
        HookDLLFunction(&Real_CreateDXGIFactory2, dxgi, "CreateDXGIFactory2", Mine_CreateDXGIFactory2);
    }
}