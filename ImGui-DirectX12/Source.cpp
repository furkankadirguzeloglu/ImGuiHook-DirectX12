#include <iostream>
#include <Windows.h>
#include "Hooks.hpp"
#include <DirectX/d3d12.h>
#include <DirectX/dxgi1_4.h>
#include <Detours/detours.h>
#include <ImGui/imgui.h>
#include <ImGui/imgui_impl_dx12.h>
#include <ImGui/imgui_impl_win32.h>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "detours.lib")

HANDLE hProcess = GetCurrentProcess();
HANDLE hThread = NULL;
HWND hWindow = NULL;
bool isInitialized = false;
bool isMenuVisible = true;

HRESULT WINAPI hkPresent(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags) {
    static ID3D12Device* pDevice = nullptr;
    static ID3D12DescriptorHeap* pBackBufferDescriptorHeap = nullptr;
    static ID3D12DescriptorHeap* pImguiRenderDescriptorHeap = nullptr;
    static ID3D12GraphicsCommandList* pCommandList = nullptr;
    static int buffersCount = 0;

    if (!pDevice) {
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&pDevice)))) {
            return oPresent(pSwapChain, SyncInterval, Flags);
        }
    }

    if (!isInitialized) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        DXGI_SWAP_CHAIN_DESC swapChainDesc;
        pSwapChain->GetDesc(&swapChainDesc);
        hWindow = swapChainDesc.OutputWindow;

        if (hWindow) {
            buffersCount = swapChainDesc.BufferCount;
            frameContext = new FrameContext[buffersCount];

            D3D12_DESCRIPTOR_HEAP_DESC descriptorImGuiRender = {};
            descriptorImGuiRender.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            descriptorImGuiRender.NumDescriptors = buffersCount;
            descriptorImGuiRender.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

            if (FAILED(pDevice->CreateDescriptorHeap(&descriptorImGuiRender, IID_PPV_ARGS(&pImguiRenderDescriptorHeap)))) {
                return oPresent(pSwapChain, SyncInterval, Flags);
            }

            ID3D12CommandAllocator* pAllocator;
            if (FAILED(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pAllocator)))) {
                return oPresent(pSwapChain, SyncInterval, Flags);
            }

            for (size_t i = 0; i < buffersCount; i++) {
                frameContext[i].pCommandAllocator = pAllocator;
            }

            if (FAILED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pAllocator, nullptr, IID_PPV_ARGS(&pCommandList))) || FAILED(pCommandList->Close())) {
                return oPresent(pSwapChain, SyncInterval, Flags);
            }

            D3D12_DESCRIPTOR_HEAP_DESC descriptorBackBuffers = {};
            descriptorBackBuffers.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            descriptorBackBuffers.NumDescriptors = buffersCount;
            descriptorBackBuffers.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            descriptorBackBuffers.NodeMask = 1;

            if (FAILED(pDevice->CreateDescriptorHeap(&descriptorBackBuffers, IID_PPV_ARGS(&pBackBufferDescriptorHeap)))) {
                return oPresent(pSwapChain, SyncInterval, Flags);
            }

            const auto rtvDescriptorSize = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = pBackBufferDescriptorHeap->GetCPUDescriptorHandleForHeapStart();

            for (size_t i = 0; i < buffersCount; i++) {
                ID3D12Resource* pBackBuffer = nullptr;
                frameContext[i].descriptorHandle = rtvHandle;
                pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));
                pDevice->CreateRenderTargetView(pBackBuffer, nullptr, rtvHandle);
                frameContext[i].pResource = pBackBuffer;
                rtvHandle.ptr += rtvDescriptorSize;
            }

            ImGui_ImplWin32_Init(hWindow);
            ImGui_ImplDX12_Init(pDevice, buffersCount, DXGI_FORMAT_R8G8B8A8_UNORM, pImguiRenderDescriptorHeap, pImguiRenderDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), pImguiRenderDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
            ImGui_ImplDX12_CreateDeviceObjects();
            SetWindowLongPtr(hWindow, GWLP_WNDPROC, (LONG_PTR)WndProc);
            isInitialized = true;
        }
    }

    if (!pCommandQueue) {
        return oPresent(pSwapChain, SyncInterval, Flags);
    }       

    if (GetAsyncKeyState(VK_INSERT) & 1) {
        isMenuVisible = !isMenuVisible;
        ImGui::GetIO().MouseDrawCursor = isMenuVisible;
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (isMenuVisible) {
        inputHandler();
        ImGui::ShowDemoWindow();
    }

    ImGui::EndFrame();

    FrameContext& CurrentFrameContext = frameContext[pSwapChain->GetCurrentBackBufferIndex()];
    CurrentFrameContext.pCommandAllocator->Reset();

    D3D12_RESOURCE_BARRIER Barrier;
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    Barrier.Transition.pResource = CurrentFrameContext.pResource;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    pCommandList->Reset(CurrentFrameContext.pCommandAllocator, nullptr);
    pCommandList->ResourceBarrier(1, &Barrier);
    pCommandList->OMSetRenderTargets(1, &CurrentFrameContext.descriptorHandle, FALSE, nullptr);
    pCommandList->SetDescriptorHeaps(1, &pImguiRenderDescriptorHeap);

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), pCommandList);
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    pCommandList->ResourceBarrier(1, &Barrier);
    pCommandList->Close();
    pCommandQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList* const*>(&pCommandList));
    return oPresent(pSwapChain, SyncInterval, Flags);
}

void WINAPI hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT NumCommandLists, ID3D12CommandList* ppCommandLists) {
    if (!pCommandQueue) {
        pCommandQueue = queue;
    }
    oExecuteCommandLists(queue, NumCommandLists, ppCommandLists);
}

void WINAPI hkDrawInstanced(ID3D12GraphicsCommandList* dCommandList, UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) {
    return oDrawInstanced(dCommandList, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void WINAPI hkDrawIndexedInstanced(ID3D12GraphicsCommandList* dCommandList, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) {
    return oDrawIndexedInstanced(dCommandList, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (isInitialized && isMenuVisible) {
        ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
        return TRUE;
    }
    switch (uMsg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_CLOSE:
        TerminateProcess(hProcess, 0);
        break;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void inputHandler() {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    for (int i = 1; i < 5; i++) {
        io.MouseDown[i] = false;
    }
}

DWORD WINAPI initializeHook(LPVOID lpParam) {
    HMODULE hModuleD3D12 = GetModuleHandle("d3d12.dll");
    if (!hModuleD3D12) {
        return FALSE;
    }

    HMODULE hModuleDXGI = GetModuleHandle("dxgi.dll");
    if (!hModuleDXGI) {
        return FALSE;
    }

    IDXGIFactory* pFactory = nullptr;
    IDXGIAdapter* pAdapter = nullptr;
    ID3D12Device* pDevice = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    ID3D12CommandAllocator* commandAllocator = nullptr;
    ID3D12GraphicsCommandList* commandList = nullptr;
    IDXGISwapChain* pSwapChain = nullptr;

    HWND hWnd;
    WNDCLASSEXA wc = { sizeof(WNDCLASSEX), CS_CLASSDC, DefWindowProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "DX", NULL };
    if (!(RegisterClassEx(&wc) && (hWnd = CreateWindow("DX", NULL, WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL)) != NULL)) {
        return FALSE;
    }

    void* pDXGIFactoryCreation = GetProcAddress(hModuleDXGI, "CreateDXGIFactory");
    if (!pDXGIFactoryCreation) {
        return FALSE;
    }

    if (((long(__stdcall*)(const IID&, void**))(pDXGIFactoryCreation))(__uuidof(IDXGIFactory), (void**)&pFactory) < 0) {
        return FALSE;
    }

    if (pFactory->EnumAdapters(0, &pAdapter) == DXGI_ERROR_NOT_FOUND) {
        return FALSE;
    }

    void* pD3D12DeviceCreate = GetProcAddress(hModuleD3D12, "D3D12CreateDevice");
    if (!pD3D12DeviceCreate) {
        return FALSE;
    }

    if (((long(__stdcall*)(IUnknown*, D3D_FEATURE_LEVEL, const IID&, void**))(pD3D12DeviceCreate))(pAdapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&pDevice) < 0) {
        return FALSE;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = 0;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    if (pDevice->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), (void**)&commandQueue) < 0) {
        return FALSE;
    }

    if (pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&commandAllocator) < 0) {
        return FALSE;
    }

    if (pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, NULL, __uuidof(ID3D12GraphicsCommandList), (void**)&commandList) < 0) {
        return FALSE;
    }

    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = hWnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (pFactory->CreateSwapChain(commandQueue, &swapChainDesc, &pSwapChain) < 0) {
        return FALSE;
    }

    static uint64_t* vTable = (uint64_t*)::calloc(150, sizeof(uint64_t));
    memcpy_s(vTable, 44 * sizeof(uint64_t), *(uint64_t**)(void*)pDevice, 44 * sizeof(uint64_t));
    memcpy_s(vTable + 44, 19 * sizeof(uint64_t), *(uint64_t**)(void*)commandQueue, 19 * sizeof(uint64_t));
    memcpy_s(vTable + 44 + 19, 9 * sizeof(uint64_t), *(uint64_t**)(void*)commandAllocator, 9 * sizeof(uint64_t));
    memcpy_s(vTable + 44 + 19 + 9, 60 * sizeof(uint64_t), *(uint64_t**)(void*)commandList, 60 * sizeof(uint64_t));
    memcpy_s(vTable + 44 + 19 + 9 + 60, 18 * sizeof(uint64_t), *(uint64_t**)(void*)pSwapChain, 18 * sizeof(uint64_t));

    oExecuteCommandLists = (ExecuteCommandLists)vTable[54];
    oPresent = (Present)vTable[140];
    oDrawInstanced = (DrawInstanced)vTable[84];
    oDrawIndexedInstanced = (DrawIndexedInstanced)vTable[85];

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(LPVOID&)oExecuteCommandLists, (PBYTE)hkExecuteCommandLists);
    DetourAttach(&(LPVOID&)oPresent, (PBYTE)hkPresent);
    DetourAttach(&(LPVOID&)oDrawInstanced, (PBYTE)hkDrawInstanced);
    DetourAttach(&(LPVOID&)oDrawIndexedInstanced, (PBYTE)hkDrawIndexedInstanced);
    DetourTransactionCommit();

    if (hWnd) {
        DestroyWindow(hWnd);
        UnregisterClass("DX", wc.hInstance);
        hWnd = nullptr;
    }
    clearVariable(pFactory);
    clearVariable(pAdapter);
    clearVariable(pDevice);
    clearVariable(commandQueue);
    clearVariable(commandAllocator);
    clearVariable(commandList);
    clearVariable(pSwapChain);
    return TRUE;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        if (!(hThread = CreateThread(NULL, 0, initializeHook, NULL, 0, NULL))){
            return FALSE;
        }
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        if (isInitialized) {
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        break;
    }
    return TRUE;
}