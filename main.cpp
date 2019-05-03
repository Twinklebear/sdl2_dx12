#include <iostream>
#include <memory>
#include <vector>
#include <array>
#include <SDL.h>
#include <SDL_syswm.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <D3Dcompiler.h>
#include <combaseapi.h>
#include <wrl.h>

#define CHECK_ERR(FN) \
	{ \
		auto res = FN; \
		if (FAILED(res)) { \
			std::cout << #FN << " failed\n"; \
			throw std::runtime_error(#FN); \
		}\
	}\

using Microsoft::WRL::ComPtr;

int win_width = 1280;
int win_height = 720;

int main(int argc, const char **argv) {
	if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
		std::cerr << "Failed to init SDL: " << SDL_GetError() << "\n";
		return -1;
	}

	SDL_Window* window = SDL_CreateWindow("SDL2 + DX12",
			SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_width, win_height,
			SDL_WINDOW_RESIZABLE);

	SDL_SysWMinfo wm_info;
	SDL_VERSION(&wm_info.version);
	SDL_GetWindowWMInfo(window, &wm_info);
	HWND win_handle = wm_info.info.win.window;

	// Enable debugging for D3D12
	ComPtr<ID3D12Debug> debug_controller;
	CHECK_ERR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)));
	debug_controller->EnableDebugLayer();

	ComPtr<IDXGIFactory2> factory;
	CHECK_ERR(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)));

	ComPtr<ID3D12Device> device;
	CHECK_ERR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)));

	// Describe and create the command queue.
	D3D12_COMMAND_QUEUE_DESC queue_desc = {};
	queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	ComPtr<ID3D12CommandQueue> cmd_queue;
	CHECK_ERR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&cmd_queue)));

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
	swap_chain_desc.BufferCount = 2;
	swap_chain_desc.Width = win_width;
	swap_chain_desc.Height = win_height;
	swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swap_chain_desc.SampleDesc.Count = 1;

	ComPtr<IDXGISwapChain3> swap_chain;
	{
		ComPtr<IDXGISwapChain1> sc;
		CHECK_ERR(factory->CreateSwapChainForHwnd(cmd_queue.Get(), win_handle,
				&swap_chain_desc, nullptr, nullptr, &sc));

		CHECK_ERR(sc.As(&swap_chain));
	}

	// Make a descriptor heap
	ComPtr<ID3D12DescriptorHeap> rtv_heap;
	D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
	rtv_heap_desc.NumDescriptors = 2;
	rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	CHECK_ERR(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)));

	const uint32_t rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	// Create render target descriptors for the swap chain's render targets
	ComPtr<ID3D12Resource> render_targets[2];
	{
		D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
		// Create a RTV for each frame.
		for (int i = 0; i < 2; ++i) {
			CHECK_ERR(swap_chain->GetBuffer(i, IID_PPV_ARGS(&render_targets[i])));
			device->CreateRenderTargetView(render_targets[i].Get(), nullptr, rtv_handle);
			rtv_handle.ptr += rtv_descriptor_size;
		}
	}

	ComPtr<ID3D12CommandAllocator> cmd_allocator;
	CHECK_ERR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_allocator)));

	// TODO: make the command list and fence

	// TODO: build the command list to clear the frame color

	bool done = false;
	while (!done) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) {
				done = true;
			}
			if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
				done = true;
			}
			if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE
					&& event.window.windowID == SDL_GetWindowID(window)) {
				done = true;
			}
			if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
				win_width = event.window.data1;
				win_height = event.window.data2;
				// TODO Resizing support
			}
		}

		// TODO: Execute the command list and sync the fence
	}

	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}

