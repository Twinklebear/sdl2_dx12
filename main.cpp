#include <iostream>
#include <thread>
#include <chrono>
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
#include <Winerror.h>

#define CHECK_ERR(FN) \
	{ \
		auto res = FN; \
		if (FAILED(res)) { \
			std::cout << #FN << " failed due to " \
				<< std::hex << res << std::endl << std::flush; \
			throw std::runtime_error(#FN); \
		}\
	}\

using Microsoft::WRL::ComPtr;

int win_width = 1280;
int win_height = 720;

const char* d3d_err_str(HRESULT res);

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

	ComPtr<ID3D12Device5> device;
	CHECK_ERR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)));

	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS5 feature_data = {0};
		CHECK_ERR(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &feature_data, sizeof(feature_data)));
		if (feature_data.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0) {
			std::cout << "DXR is available\n";
		} else {
			std::cout << "DXR is missing, exiting\n";
			return 1;
		}
	}

	// Describe and create the command queue.
	D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
	queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	ComPtr<ID3D12CommandQueue> cmd_queue;
	CHECK_ERR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&cmd_queue)));

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {0};
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
	D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
	rtv_heap_desc.NumDescriptors = 2;
	rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	CHECK_ERR(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)));

	const uint32_t rtv_descriptor_size =
		device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	// Create render target descriptors for the swap chain's render targets
	ComPtr<ID3D12Resource> render_targets[2];
	{
		D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
		// Create a RTV for each frame
		for (int i = 0; i < 2; ++i) {
			CHECK_ERR(swap_chain->GetBuffer(i, IID_PPV_ARGS(&render_targets[i])));
			device->CreateRenderTargetView(render_targets[i].Get(), nullptr, rtv_handle);
			rtv_handle.ptr += rtv_descriptor_size;
		}
	}

	ComPtr<ID3D12CommandAllocator> cmd_allocator;
	CHECK_ERR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(&cmd_allocator)));

	// Load assets

	// Make an empty root signature
	ComPtr<ID3D12RootSignature> root_signature;	
	{
		D3D12_ROOT_SIGNATURE_DESC desc = {0};
		desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		CHECK_ERR(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
		CHECK_ERR(device->CreateRootSignature(0, signature->GetBufferPointer(),
					signature->GetBufferSize(), IID_PPV_ARGS(&root_signature)));
	}

	// Setup the pipeline state and the shaders which will be used for it
	ComPtr<ID3D12PipelineState> pipeline_state;
	{
		ComPtr<ID3DBlob> compiler_errors;
		ComPtr<ID3DBlob> vertex_shader;
		ComPtr<ID3DBlob> pixel_shader;
		const uint32_t compile_flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
		if (FAILED(D3DCompileFromFile(L"res/shader.hlsl", nullptr, nullptr,
				"VSMain", "vs_5_0", compile_flags, 0, &vertex_shader, &compiler_errors)))
		{
			std::cout << "Failed to compile vertex shader, err msg: "
				<< reinterpret_cast<char*>(compiler_errors->GetBufferPointer())
				<< std::endl << std::flush;
			throw std::runtime_error("VS did not compile");
		}
		if (FAILED(D3DCompileFromFile(L"res/shader.hlsl", nullptr, nullptr,
				"PSMain", "ps_5_0", compile_flags, 0, &pixel_shader, &compiler_errors)))
		{
			std::cout << "Failed to compile pixel shader, err msg: "
				<< reinterpret_cast<char*>(compiler_errors->GetBufferPointer())
				<< std::endl << std::flush;
			throw std::runtime_error("PS did not compile");
		}

		// Specify the vertex data layout
		std::array<D3D12_INPUT_ELEMENT_DESC, 2> vertex_layout = {
			D3D12_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
				D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
			D3D12_INPUT_ELEMENT_DESC{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
				D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
		};

		// Create the graphic pipeline state description
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {0};
		desc.pRootSignature = root_signature.Get();

		desc.VS.pShaderBytecode = vertex_shader->GetBufferPointer();
		desc.VS.BytecodeLength = vertex_shader->GetBufferSize();
		desc.PS.pShaderBytecode = pixel_shader->GetBufferPointer();
		desc.PS.BytecodeLength = pixel_shader->GetBufferSize();

		desc.BlendState.AlphaToCoverageEnable = FALSE;
		desc.BlendState.IndependentBlendEnable = FALSE;
		{
			const D3D12_RENDER_TARGET_BLEND_DESC rt_blend_desc = {
				false, false,
				D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
				D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
				D3D12_LOGIC_OP_NOOP,
				D3D12_COLOR_WRITE_ENABLE_ALL,
			};
			for (int i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
				desc.BlendState.RenderTarget[i] = rt_blend_desc;
			}
		}

		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		desc.RasterizerState.FrontCounterClockwise = FALSE;
		desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.RasterizerState.MultisampleEnable = FALSE;
		desc.RasterizerState.AntialiasedLineEnable = FALSE;
		desc.RasterizerState.ForcedSampleCount = 0;
		desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		desc.SampleMask = UINT_MAX;
		desc.DepthStencilState.DepthEnable = false;
		desc.DepthStencilState.StencilEnable = false;

		desc.InputLayout.pInputElementDescs = vertex_layout.data();
		desc.InputLayout.NumElements = vertex_layout.size();
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;

		CHECK_ERR(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline_state)));
	}

	// Make the command list
	ComPtr<ID3D12GraphicsCommandList4> cmd_list;
	CHECK_ERR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_allocator.Get(),
				pipeline_state.Get(), IID_PPV_ARGS(&cmd_list)));
	CHECK_ERR(cmd_list->Close());

	// Create the VBO containing the triangle data
	ComPtr<ID3D12Resource> vbo, upload, bottom_level_as, top_level_as;
	D3D12_VERTEX_BUFFER_VIEW vbo_view;
	{
		const std::array<float, 24> vertex_data = {
			0, 0.5, 0, 1,
			1, 0, 0, 1,

			0.5, -0.5, 0, 1,
			0, 1, 0, 1,

			-0.5, -0.5, 0, 1,
			0, 0, 1, 1
		};

		// TODO: The example mentions that using the upload heap to transfer data
		// like this is not recommended b/c it will need to recopy it each time the GPU
		// needs it. Look into default heap usage in the future. Should be some way
		// to upload into the GPU memory so it is fixed there in VRAM. Based on the doc
		// this would be a two step process: Make a default heap and and upload heap,
		// copy data from CPU to the upload heap, then from the upload heap into the
		// default heap (which is memory resident on the GPU)
		D3D12_HEAP_PROPERTIES props = {0};
		props.Type = D3D12_HEAP_TYPE_UPLOAD;
		props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC res_desc = {0};
		res_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		res_desc.Width = sizeof(float) * vertex_data.size();
		res_desc.Height = 1;
		res_desc.DepthOrArraySize = 1;
		res_desc.MipLevels = 1;
		res_desc.Format = DXGI_FORMAT_UNKNOWN;
		res_desc.SampleDesc.Count = 1;
		res_desc.SampleDesc.Quality = 0;
		res_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		res_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		// Place the vertex data in an upload heap first, then do a GPU-side copy
		// into a default heap (resident in VRAM)
		CHECK_ERR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
					&res_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
					nullptr, IID_PPV_ARGS(&upload)));

		// Now copy the data into the upload heap
		uint8_t *mapping = nullptr;
		D3D12_RANGE read_range = {0};
		CHECK_ERR(upload->Map(0, &read_range, reinterpret_cast<void**>(&mapping)));
		std::memcpy(mapping, vertex_data.data(), sizeof(float) * vertex_data.size());
		upload->Unmap(0, nullptr);

		// Now setup the GPU-side heap to hold the verts for rendering
		props.Type = D3D12_HEAP_TYPE_DEFAULT;
		CHECK_ERR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
					&res_desc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
					nullptr, IID_PPV_ARGS(&vbo)));

		CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), pipeline_state.Get()));

		// Transition vbo buffer to a copy dest buffer
		D3D12_RESOURCE_BARRIER res_barrier;
		res_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		res_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		res_barrier.Transition.pResource = vbo.Get();
		res_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		res_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		res_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

		cmd_list->ResourceBarrier(1, &res_barrier);

		// Now enqueue the copy
		cmd_list->CopyResource(vbo.Get(), upload.Get());

		// Transition the vbo back to vertex and constant buffer state
		res_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		res_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;

		cmd_list->ResourceBarrier(1, &res_barrier);

		// Now build the bottom level acceleration structure on the triangle
		D3D12_RAYTRACING_GEOMETRY_DESC rt_geom_desc = {0};
		rt_geom_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		rt_geom_desc.Triangles.VertexBuffer.StartAddress = vbo->GetGPUVirtualAddress();
		rt_geom_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 8;
		rt_geom_desc.Triangles.VertexCount = 3;
		rt_geom_desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		// No index buffer for now
		rt_geom_desc.Triangles.IndexBuffer = 0;
		rt_geom_desc.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
		rt_geom_desc.Triangles.IndexCount = 0;
		rt_geom_desc.Triangles.Transform3x4 = 0;
		rt_geom_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

		// Determine bound of much memory the accel builder may need and allocate it
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS as_inputs = {0};
		as_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		as_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		as_inputs.NumDescs = 1;
		as_inputs.pGeometryDescs = &rt_geom_desc;
		as_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild_info = {0};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&as_inputs, &prebuild_info);

		// The buffer sizes must be aligned to 256 bytes
		prebuild_info.ResultDataMaxSizeInBytes +=
			prebuild_info.ResultDataMaxSizeInBytes % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
		// TODO: Not sure the scratch needs this alignment
		prebuild_info.ScratchDataSizeInBytes +=
			prebuild_info.ScratchDataSizeInBytes % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;

		std::cout << "Result AS will use at most " << prebuild_info.ResultDataMaxSizeInBytes
			<< " bytes, and scratch of " << prebuild_info.ScratchDataSizeInBytes << " bytes\n";

		// Allocate the buffer for the final bottom level AS
		res_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		res_desc.Alignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
		res_desc.Width = prebuild_info.ResultDataMaxSizeInBytes;
		res_desc.Height = 1;
		res_desc.DepthOrArraySize = 1;
		res_desc.MipLevels = 1;
		res_desc.Format = DXGI_FORMAT_UNKNOWN;
		res_desc.SampleDesc.Count = 1;
		res_desc.SampleDesc.Quality = 0;
		res_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		CHECK_ERR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
			&res_desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			nullptr, IID_PPV_ARGS(&bottom_level_as)));

		{
			// Allocate the scratch space
			ComPtr<ID3D12Resource> build_scratch;
			res_desc.Width = prebuild_info.ScratchDataSizeInBytes;
			CHECK_ERR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
				&res_desc, D3D12_RESOURCE_STATE_COMMON,
				nullptr, IID_PPV_ARGS(&build_scratch)));

			// Now build the bottom level acceleration structure
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc;
			build_desc.Inputs = as_inputs;
			build_desc.DestAccelerationStructureData = bottom_level_as->GetGPUVirtualAddress();
			build_desc.ScratchAccelerationStructureData = build_scratch->GetGPUVirtualAddress();
			cmd_list->BuildRaytracingAccelerationStructure(&build_desc, 0, NULL);

			// Insert a barrier to wait for the bottom level AS to complete before the top level
			// build is started
			D3D12_RESOURCE_BARRIER build_barrier;
			build_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			build_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			build_barrier.UAV.pResource = bottom_level_as.Get();
			cmd_list->ResourceBarrier(1, &build_barrier);
		}

		CHECK_ERR(cmd_list->Close());

		// Execute the command list to do the copy
		std::array<ID3D12CommandList*, 1> cmd_lists = {cmd_list.Get()};
		cmd_queue->ExecuteCommandLists(cmd_lists.size(), cmd_lists.data());

		// Setup the vertex buffer view
		vbo_view.BufferLocation = vbo->GetGPUVirtualAddress();
		vbo_view.StrideInBytes = sizeof(float) * 8;
		vbo_view.SizeInBytes = sizeof(float) * vertex_data.size();
	}

	// Create the fence
	ComPtr<ID3D12Fence> fence;
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	int fence_value = 1;

	HANDLE fence_evt = CreateEvent(nullptr, false, false, nullptr);
	if (fence_evt == nullptr) {
		std::cout << "Failed to make fence event\n";
		throw std::runtime_error("Failed to make fence event");
	}
	{
		// Sync with the fence to wait for the assets to upload
		const uint32_t signal_val = fence_value++;
		CHECK_ERR(cmd_queue->Signal(fence.Get(), signal_val));

		if (fence->GetCompletedValue() < signal_val) {
			CHECK_ERR(fence->SetEventOnCompletion(signal_val, fence_evt));
			WaitForSingleObject(fence_evt, INFINITE);
		}

		// We know the gpu-side data copy is done now, so release the upload buffer
		upload = nullptr;
	}

	D3D12_RECT screen_bounds = {0};
	screen_bounds.right = win_width;
	screen_bounds.bottom = win_height;

	D3D12_VIEWPORT viewport = {0};
	viewport.Width = static_cast<float>(win_width);
	viewport.Height = static_cast<float>(win_height);
	viewport.MinDepth = D3D12_MIN_DEPTH;
	viewport.MaxDepth = D3D12_MAX_DEPTH;

	int back_buffer_idx = swap_chain->GetCurrentBackBufferIndex();
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

				for (auto &rt : render_targets) {
					rt = nullptr;
				}

				DXGI_SWAP_CHAIN_DESC desc = {0};
				swap_chain->GetDesc(&desc);
				swap_chain->ResizeBuffers(2, win_width, win_height,
						desc.BufferDesc.Format, desc.Flags);
				back_buffer_idx = swap_chain->GetCurrentBackBufferIndex();

				// Update the render target handles
				D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
				for (int i = 0; i < 2; ++i) {
					CHECK_ERR(swap_chain->GetBuffer(i, IID_PPV_ARGS(&render_targets[i])));
					device->CreateRenderTargetView(render_targets[i].Get(), nullptr, rtv_handle);
					rtv_handle.ptr += rtv_descriptor_size;
				}

				screen_bounds.right = win_width;
				screen_bounds.bottom = win_height;
				viewport.Width = static_cast<float>(win_width);
				viewport.Height = static_cast<float>(win_height);
			}
		}

		// Build the command list to clear the frame color
		CHECK_ERR(cmd_allocator->Reset());

		CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), pipeline_state.Get()));

		cmd_list->SetGraphicsRootSignature(root_signature.Get());
		cmd_list->RSSetViewports(1, &viewport);
		cmd_list->RSSetScissorRects(1, &screen_bounds);

		// Back buffer will be used as render target
		{
			D3D12_RESOURCE_BARRIER res_barrier;
			res_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			res_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			res_barrier.Transition.pResource = render_targets[back_buffer_idx].Get();
			res_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			res_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			res_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			cmd_list->ResourceBarrier(1, &res_barrier);
		}

		D3D12_CPU_DESCRIPTOR_HANDLE render_target = rtv_heap->GetCPUDescriptorHandleForHeapStart();
		render_target.ptr += rtv_descriptor_size * back_buffer_idx;
		cmd_list->OMSetRenderTargets(1, &render_target, false, nullptr);

		const std::array<float, 4> clear_color = {0.f, 0.2f, 0.4f, 1.f};
		cmd_list->ClearRenderTargetView(render_target, clear_color.data(), 0, nullptr);
#if 1
		// Rasterize the triangle
		cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmd_list->IASetVertexBuffers(0, 1, &vbo_view);
		cmd_list->DrawInstanced(3, 1, 0, 0);
#else
		// TODO: Ray trace it
#endif

		// Back buffer will now be used to present
		{
			D3D12_RESOURCE_BARRIER res_barrier;
			res_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			res_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			res_barrier.Transition.pResource = render_targets[back_buffer_idx].Get();
			res_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			res_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			res_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			cmd_list->ResourceBarrier(1, &res_barrier);
		}

		CHECK_ERR(cmd_list->Close());

		// Execute the command list and present
		std::array<ID3D12CommandList*, 1> cmd_lists = {cmd_list.Get()};
		cmd_queue->ExecuteCommandLists(cmd_lists.size(), cmd_lists.data());
		CHECK_ERR(swap_chain->Present(1, 0));

		// Sync with the fence to wait for the frame to be presented
		const uint32_t signal_val = fence_value++;
		CHECK_ERR(cmd_queue->Signal(fence.Get(), signal_val));

		if (fence->GetCompletedValue() < signal_val) {
			CHECK_ERR(fence->SetEventOnCompletion(signal_val, fence_evt));
			WaitForSingleObject(fence_evt, INFINITE);
		}

		// Update the back buffer index to the new back buffer now that the
		// swap chain has swapped.
		back_buffer_idx = swap_chain->GetCurrentBackBufferIndex();
	}

	CloseHandle(fence_evt);
	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}

