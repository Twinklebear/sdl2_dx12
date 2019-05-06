#include <iostream>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <array>
#include <iomanip>
#include <string>
#include <sstream>
#include <SDL.h>
#include <SDL_syswm.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <D3Dcompiler.h>
#include <combaseapi.h>
#include <wrl.h>
#include <Winerror.h>

#include "rt_shader_embedded_dxil.h"

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

// Borrowed from https://github.com/Microsoft/DirectX-Graphics-Samples
void PrintStateObjectDesc(const D3D12_STATE_OBJECT_DESC* desc);

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

	// Setup the pipeline state and the shaders which will be used for rasterizing the triangle
	
	// Make the command list
	ComPtr<ID3D12GraphicsCommandList4> cmd_list;
	CHECK_ERR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_allocator.Get(),
				nullptr, IID_PPV_ARGS(&cmd_list)));
	CHECK_ERR(cmd_list->Close());

	// Create the VBO containing the triangle data
	ComPtr<ID3D12Resource> vbo, upload;
	ComPtr<ID3D12Resource> bottom_level_as, bottom_scratch;
	ComPtr<ID3D12Resource> top_level_as, top_scratch, instances;
	ComPtr<ID3D12StateObject> rt_state_object;
	ComPtr<ID3D12Resource> rt_output_img;
	ComPtr<ID3D12DescriptorHeap> rt_shader_res_heap;
	ComPtr<ID3D12Resource> rt_shader_table;
	ComPtr<ID3D12RootSignature> rt_root_signature;
	
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
		CHECK_ERR(upload->Map(0, NULL, reinterpret_cast<void**>(&mapping)));
		std::memcpy(mapping, vertex_data.data(), sizeof(float) * vertex_data.size());
		upload->Unmap(0, nullptr);

		// Now setup the GPU-side heap to hold the verts for rendering
		props.Type = D3D12_HEAP_TYPE_DEFAULT;
		CHECK_ERR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
					&res_desc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
					nullptr, IID_PPV_ARGS(&vbo)));

		CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), nullptr));

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
		res_barrier.Transition.StateAfter =
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
			| D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

		cmd_list->ResourceBarrier(1, &res_barrier);

		// Setup the vertex buffer view
		vbo_view.BufferLocation = vbo->GetGPUVirtualAddress();
		vbo_view.StrideInBytes = sizeof(float) * 8;
		vbo_view.SizeInBytes = sizeof(float) * vertex_data.size();

		// ===============================
		// Build the RT acceleration structures
		// ===============================

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
		prebuild_info.ResultDataMaxSizeInBytes += D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT -
			prebuild_info.ResultDataMaxSizeInBytes % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
		prebuild_info.ScratchDataSizeInBytes += D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT -
			prebuild_info.ScratchDataSizeInBytes % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
	
		std::cout << "Bottom level AS will use at most " << prebuild_info.ResultDataMaxSizeInBytes
			<< " bytes, and scratch of " << prebuild_info.ScratchDataSizeInBytes << " bytes\n";

		// Allocate the buffer for the final bottom level AS
		res_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		res_desc.Alignment = 0;
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

		// Allocate the scratch space for the bottom level build
		res_desc.Width = prebuild_info.ScratchDataSizeInBytes;
		CHECK_ERR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
			&res_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			nullptr, IID_PPV_ARGS(&bottom_scratch)));

		// Now build the bottom level acceleration structure
		{
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc = {0};
			build_desc.Inputs = as_inputs;
			build_desc.DestAccelerationStructureData = bottom_level_as->GetGPUVirtualAddress();
			build_desc.ScratchAccelerationStructureData = bottom_scratch->GetGPUVirtualAddress();
			cmd_list->BuildRaytracingAccelerationStructure(&build_desc, 0, NULL);

			// Insert a barrier to wait for the bottom level AS to complete before the top level
			// build is started
			D3D12_RESOURCE_BARRIER build_barrier = {0};
			build_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			build_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			build_barrier.UAV.pResource = bottom_level_as.Get();
			cmd_list->ResourceBarrier(1, &build_barrier);
		}

		// Now build the top-level accel. structure over our one instantiation of the bottom level AS
		
		// Determine the space required for the top-level AS build
		as_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		as_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		as_inputs.NumDescs = 1;
		// Note: we don't need to give it the instance descriptors to estimate the size, just
		// when we want to build the data. Just unset the geom descs. now since we're re-using it
		as_inputs.pGeometryDescs = 0;
		as_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

		device->GetRaytracingAccelerationStructurePrebuildInfo(&as_inputs, &prebuild_info);
		// The buffer sizes must be aligned to 256 bytes
		prebuild_info.ResultDataMaxSizeInBytes += D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT -
			prebuild_info.ResultDataMaxSizeInBytes % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
		// TODO: Not sure the scratch needs this alignment
		prebuild_info.ScratchDataSizeInBytes += D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT -
			prebuild_info.ScratchDataSizeInBytes % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;

		std::cout << "Top level AS will use at most " << prebuild_info.ResultDataMaxSizeInBytes
			<< " bytes, and scratch of " << prebuild_info.ScratchDataSizeInBytes << " bytes\n";

		res_desc.Width = prebuild_info.ResultDataMaxSizeInBytes;
		CHECK_ERR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
			&res_desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			nullptr, IID_PPV_ARGS(&top_level_as)));

		res_desc.Width = prebuild_info.ScratchDataSizeInBytes;
		CHECK_ERR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
			&res_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			nullptr, IID_PPV_ARGS(&top_scratch)));

		// Allocate space for the instances as well
		res_desc.Width = sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
		// Align it as well to the constant buffer size
		res_desc.Width += D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT -
			res_desc.Width % D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT;
		// We want to write from the CPU to this buffer so place in the upload heap
		props.Type = D3D12_HEAP_TYPE_UPLOAD;
		res_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
		CHECK_ERR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
			&res_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr, IID_PPV_ARGS(&instances)));

		// Fill out the instance information
		{
			D3D12_RAYTRACING_INSTANCE_DESC *buf;
			instances->Map(0, nullptr, reinterpret_cast<void**>(&buf));

			buf->InstanceID = 0;
			// TODO: does this mean you can do per-instance hit shaders?
			buf->InstanceContributionToHitGroupIndex = 0;
			buf->Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
			buf->AccelerationStructure = bottom_level_as->GetGPUVirtualAddress();
			buf->InstanceMask = 0xff;

			// WILL Note: these matrices (and D3D generally?) are row major
			// this is identity now so we don't really car
			std::memset(buf->Transform, 0, sizeof(buf->Transform));
			buf->Transform[0][0] = 1.f;
			buf->Transform[1][1] = 1.f;
			buf->Transform[2][2] = 1.f;
			
			instances->Unmap(0, nullptr);
		}

		// Now build the top level acceleration structure
		{
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc = {0};
			as_inputs.InstanceDescs = instances->GetGPUVirtualAddress();
			build_desc.Inputs = as_inputs;
			build_desc.DestAccelerationStructureData = top_level_as->GetGPUVirtualAddress();
			build_desc.ScratchAccelerationStructureData = top_scratch->GetGPUVirtualAddress();
			cmd_list->BuildRaytracingAccelerationStructure(&build_desc, 0, NULL);

			// Insert a barrier to wait for the bottom level AS to complete before the top level
			// build is started
			D3D12_RESOURCE_BARRIER build_barrier = {0};
			build_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			build_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			build_barrier.UAV.pResource = top_level_as.Get();
			cmd_list->ResourceBarrier(1, &build_barrier);

			// We won't need to run anything else on the command list until render time,
			// so submit this work now
			CHECK_ERR(cmd_list->Close());

			// Execute the command list to do the copy
			std::array<ID3D12CommandList*, 1> cmd_lists = { cmd_list.Get() };
			cmd_queue->ExecuteCommandLists(cmd_lists.size(), cmd_lists.data());
		}

		// ===============================
		// Build the RT pipeline
		// ===============================

		// Load the DXIL shader library
		D3D12_SHADER_BYTECODE dxil_bytecode = { 0 };
		dxil_bytecode.pShaderBytecode = rt_shader_dxil;
		dxil_bytecode.BytecodeLength = sizeof(rt_shader_dxil);

		// Setup the exports for the shader library
		D3D12_DXIL_LIBRARY_DESC shader_lib = { 0 };
		std::vector<D3D12_EXPORT_DESC> exports;
		std::vector<LPCWSTR> shader_exported_fcns;
		const std::vector<std::wstring> export_fcn_names = {
			L"RayGen", L"Miss", L"ClosestHit"
		};
		for (const auto &fn : export_fcn_names) {
			D3D12_EXPORT_DESC shader_export = { 0 };
			shader_export.ExportToRename = nullptr;
			shader_export.Flags = D3D12_EXPORT_FLAG_NONE;
			shader_export.Name = fn.c_str();
			exports.push_back(shader_export);
			shader_exported_fcns.push_back(fn.c_str());
		}
		shader_lib.DXILLibrary = dxil_bytecode;
		shader_lib.NumExports = exports.size();
		shader_lib.pExports = exports.data();

		// Build the hit group which uses our shader library
		D3D12_HIT_GROUP_DESC hit_group = { 0 };
		hit_group.HitGroupExport = L"HitGroup";
		hit_group.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
		hit_group.ClosestHitShaderImport = L"ClosestHit";


		// Make the shader config which defines the maximum size in bytes for the ray
		// payload and attribute structure
		D3D12_RAYTRACING_SHADER_CONFIG shader_desc = { 0 };
		// Payload will just be a float4 color + z
		shader_desc.MaxPayloadSizeInBytes = 4 * sizeof(float);
		// Attribute size is just the float2 barycentrics
		shader_desc.MaxAttributeSizeInBytes = 2 * sizeof(float);

		// Create the root signature for this shader library
		// Note that the closest hit and miss shaders don't need one since they
		// don't make use of a local root signature (no reads from buffers/textures)
		{
			std::vector<D3D12_ROOT_PARAMETER> rt_params;
			// The raygen program takes two parameters:
			// the UAV representing the output image buffer
			// the SRV representing the top-level acceleration structure
			// TODO WILL: Even though this is just one descriptor each, I think this
			// must always be passed as a descriptor table to allow for per hit group params
			D3D12_ROOT_PARAMETER param = { 0 };
			param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			// UAV param for the output image buffer
			D3D12_DESCRIPTOR_RANGE descrip_range_uav = { 0 };
			descrip_range_uav.BaseShaderRegister = 0;
			descrip_range_uav.NumDescriptors = 1;
			descrip_range_uav.RegisterSpace = 0;
			descrip_range_uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			descrip_range_uav.OffsetInDescriptorsFromTableStart = 0;
			
			param.DescriptorTable.NumDescriptorRanges = 1;
			param.DescriptorTable.pDescriptorRanges = &descrip_range_uav;
			rt_params.push_back(param);

			// SRV param for the output image buffer, which is the second entry in the table(?)
			D3D12_DESCRIPTOR_RANGE descrip_range_srv = { 0 };
			descrip_range_srv.BaseShaderRegister = 0;
			descrip_range_srv.NumDescriptors = 1;
			descrip_range_srv.RegisterSpace = 0;
			descrip_range_srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			// second entry so offset 1(?)
			descrip_range_srv.OffsetInDescriptorsFromTableStart = 1;
			
			param.DescriptorTable.pDescriptorRanges = &descrip_range_srv;
			rt_params.push_back(param);

			D3D12_ROOT_SIGNATURE_DESC root_desc = { 0 };
			root_desc.NumParameters = rt_params.size();
			root_desc.pParameters = rt_params.data();
			// RT root signatures are local (to the shader?)
			root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

			// Create the root signature from the descriptor
			ComPtr<ID3DBlob> signature_blob;
			ComPtr<ID3DBlob> err_blob;
			auto res = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
				&signature_blob, &err_blob);
			if (FAILED(res)) {
				std::cout << "Failed to serialize root signature: " << err_blob->GetBufferPointer() << "\n";
				throw std::runtime_error("Failed to serialize root signature");
			}

			CHECK_ERR(device->CreateRootSignature(0, signature_blob->GetBufferPointer(),
				signature_blob->GetBufferSize(), IID_PPV_ARGS(&rt_root_signature)));
		}

		// Now we can build the raytracing pipeline. It's made of a bunch of subobjects that
		// describe the shader code libraries, hit groups, root signature associations and
		// some other config stuff
		std::vector<D3D12_STATE_SUBOBJECT> subobjects;
		subobjects.resize(9);
		size_t current_subobj = 0;
		{
			D3D12_STATE_SUBOBJECT dxil_libs = { 0 };
			dxil_libs.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
			dxil_libs.pDesc = &shader_lib;
			subobjects[current_subobj++] = dxil_libs;
		}
		{
			D3D12_STATE_SUBOBJECT hit_grp_obj = { 0 };
			hit_grp_obj.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
			hit_grp_obj.pDesc = &hit_group;
			subobjects[current_subobj++] = hit_grp_obj;
		}
		{
			D3D12_STATE_SUBOBJECT shader_cfg = { 0 };
			shader_cfg.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
			shader_cfg.pDesc = &shader_desc;
			subobjects[current_subobj++] = shader_cfg;
		}

		D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION shader_paylod_assoc = { 0 };
		shader_paylod_assoc.NumExports = shader_exported_fcns.size();
		shader_paylod_assoc.pExports = shader_exported_fcns.data();
		// Associate with the raytracing shader config subobject
		shader_paylod_assoc.pSubobjectToAssociate = &subobjects[current_subobj - 1];
		{
			D3D12_STATE_SUBOBJECT payload_subobj = { 0 };
			payload_subobj.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
			payload_subobj.pDesc = &shader_paylod_assoc;
			subobjects[current_subobj++] = payload_subobj;
		}

		// The root signature needs two subobjects: one to declare it, and one to associate it
		// with a set of symbols
		D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION root_sig_assoc = { 0 };
		D3D12_LOCAL_ROOT_SIGNATURE rt_local_root_sig;
		rt_local_root_sig.pLocalRootSignature = rt_root_signature.Get();
		{
			// Declare the root signature
			D3D12_STATE_SUBOBJECT root_sig_obj = { 0 };
			root_sig_obj.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
			root_sig_obj.pDesc = &rt_local_root_sig;
			subobjects[current_subobj++] = root_sig_obj;

			root_sig_assoc.NumExports = shader_exported_fcns.size();
			root_sig_assoc.pExports = shader_exported_fcns.data();
			root_sig_assoc.pSubobjectToAssociate = &subobjects[current_subobj - 1];
			
			// Associate it with the symbols
			D3D12_STATE_SUBOBJECT root_assoc = { 0 };
			root_assoc.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
			root_assoc.pDesc = &root_sig_assoc;
			subobjects[current_subobj++] = root_assoc;
		}

		// The pipeline construction for some reason always needs an empty global and local root signature
		// according to the samples and NV wrapper. TODO WILL: Why? What are these for? Global scene stuff
		// which is set across all shaders??
		// Make the dummy global and local root signatures
		ComPtr<ID3D12RootSignature> dummy_global, dummy_local;
		{
			D3D12_ROOT_SIGNATURE_DESC desc = { 0 };
			desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

			ComPtr<ID3DBlob> signature;
			ComPtr<ID3DBlob> error;
			CHECK_ERR(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
			CHECK_ERR(device->CreateRootSignature(0, signature->GetBufferPointer(),
				signature->GetBufferSize(), IID_PPV_ARGS(&dummy_global)));

			desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
			CHECK_ERR(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
			CHECK_ERR(device->CreateRootSignature(0, signature->GetBufferPointer(),
				signature->GetBufferSize(), IID_PPV_ARGS(&dummy_local)));
		}
		
		// Add them to the state
		D3D12_GLOBAL_ROOT_SIGNATURE dummy_global_sig;
		dummy_global_sig.pGlobalRootSignature = dummy_global.Get();
		
		D3D12_LOCAL_ROOT_SIGNATURE dummy_local_sig;
		dummy_local_sig.pLocalRootSignature = dummy_local.Get();
		{
			D3D12_STATE_SUBOBJECT dummy_sig = { 0 };
			dummy_sig.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
			dummy_sig.pDesc = &dummy_global_sig;
			subobjects[current_subobj++] = dummy_sig;
			
			dummy_sig.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
			dummy_sig.pDesc = &dummy_local_sig;
			subobjects[current_subobj++] = dummy_sig;
		}

		// Add a subobject for the ray tracing pipeline configuration
		D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_cfg = { 0 };
		pipeline_cfg.MaxTraceRecursionDepth = 1;

		// Add to the subobjects
		{
			D3D12_STATE_SUBOBJECT pipeline_subobj = { 0 };
			pipeline_subobj.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
			pipeline_subobj.pDesc = &pipeline_cfg;
			subobjects[current_subobj++] = pipeline_subobj;
		}

		std::cout << "pipeline has " << current_subobj << " elements" << std::endl;
		// Describe the set of subobjects in our raytracing pipeline
		D3D12_STATE_OBJECT_DESC pipeline_desc = { 0 };
		pipeline_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
		pipeline_desc.NumSubobjects = current_subobj;
		pipeline_desc.pSubobjects = subobjects.data();
		
		PrintStateObjectDesc(&pipeline_desc);

		CHECK_ERR(device->CreateStateObject(&pipeline_desc, IID_PPV_ARGS(&rt_state_object)));

		// ===============================
		// Create the output texture for the ray tracer
		// ===============================
		{
			D3D12_HEAP_PROPERTIES props = { 0 };
			props.Type = D3D12_HEAP_TYPE_DEFAULT;
			props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			D3D12_RESOURCE_DESC res_desc = { 0 };
			res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			res_desc.Width = win_width;
			res_desc.Height = win_height;
			res_desc.DepthOrArraySize = 1;
			res_desc.MipLevels = 1;
			res_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			res_desc.SampleDesc.Count = 1;
			res_desc.SampleDesc.Quality = 0;
			res_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

			CHECK_ERR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
				&res_desc, D3D12_RESOURCE_STATE_COPY_SOURCE,
				nullptr, IID_PPV_ARGS(&rt_output_img)));
		}

		// ===============================
		// Create the shader resource heap
		// the resource heap has the pointers/views things to our output image buffer
		// and the top level acceleration structure
		// ===============================
		{
			D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { 0 };
			heap_desc.NumDescriptors = 2;
			heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			CHECK_ERR(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rt_shader_res_heap)));

			// Write the descriptors into the heap
			D3D12_CPU_DESCRIPTOR_HANDLE heap_handle = rt_shader_res_heap->GetCPUDescriptorHandleForHeapStart();

			D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = { 0 };
			uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device->CreateUnorderedAccessView(rt_output_img.Get(), nullptr, &uav_desc, heap_handle);

			// Write the TLAS after the output image in the heap
			heap_handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			D3D12_SHADER_RESOURCE_VIEW_DESC tlas_desc = { 0 };
			tlas_desc.Format = DXGI_FORMAT_UNKNOWN;
			tlas_desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
			tlas_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			tlas_desc.RaytracingAccelerationStructure.Location = top_level_as->GetGPUVirtualAddress();
			device->CreateShaderResourceView(nullptr, &tlas_desc, heap_handle);
		}

		// ===============================
		// Create the shader binding table
		// This is a table of pointers to the shader code and their respective descriptor heaps
		// ===============================
		D3D12_GPU_DESCRIPTOR_HANDLE res_heap_handle = rt_shader_res_heap->GetGPUDescriptorHandleForHeapStart();

		ID3D12StateObjectProperties *rt_pipeline_props = nullptr;
		rt_state_object->QueryInterface(&rt_pipeline_props);
		
		// An SBT entry is the program ID along with a set of params for the program.
		// the params are either 8 byte pointers (or the example mentions 4 byte constants, how to set or use those?)
		// Furthermore, the stride between elements is specified per-group (ray gen, hit, miss, etc) so it
		// must be padded to the largest size required by any individual entry. Note the order also matters for
		// and should match the instance contribution to hit group index.
		// In this example it's simple: our ray gen program takes a single ptr arg to the rt_shader_res_heap,
		// and our others don't take arguments at all
		{
			// 3 shaders and one that takes a single pointer param (ray-gen). However, each shader
			// binding table in the dispatch rays must have its address start at a 64byte alignment,
			// and use a 32byte stride. So pad these out to meet those requirements by making each
			// entry 64 bytes
			uint32_t sbt_table_size = 4 * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
			// What's the alignment requirement here?
			sbt_table_size += D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT -
				sbt_table_size % D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
			std::cout << "SBT is " << sbt_table_size << " bytes\n";

			D3D12_HEAP_PROPERTIES props = { 0 };
			props.Type = D3D12_HEAP_TYPE_UPLOAD;
			props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			D3D12_RESOURCE_DESC res_desc = { 0 };
			res_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			res_desc.Width = sbt_table_size;
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
				nullptr, IID_PPV_ARGS(&rt_shader_table)));

			// Map the SBT and write our shader data and param info to it
			uint8_t *sbt_map = nullptr;
			CHECK_ERR(rt_shader_table->Map(0, nullptr, reinterpret_cast<void**>(&sbt_map)));

			// First we write the ray-gen shader identifier, followed by the ptr to its descriptor heap
			std::memcpy(sbt_map, rt_pipeline_props->GetShaderIdentifier(L"RayGen"),
				D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
			sbt_map += D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
			
			std::memcpy(sbt_map, &res_heap_handle.ptr, sizeof(uint64_t));
			// Each entry must start at an alignment of 32bytes, so offset by the required alignment
			sbt_map += D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;

			std::memcpy(sbt_map, rt_pipeline_props->GetShaderIdentifier(L"Miss"),
				D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
			sbt_map += 2 * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
			
			std::memcpy(sbt_map, rt_pipeline_props->GetShaderIdentifier(L"HitGroup"),
				D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			rt_shader_table->Unmap(0, nullptr);
		}
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
		// and the AS scratch buffers
		upload = nullptr;
		bottom_scratch = nullptr;
		top_scratch = nullptr;
	}

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

				// Resize the output buffer
				{
					rt_output_img = nullptr;
					D3D12_HEAP_PROPERTIES props = { 0 };
					props.Type = D3D12_HEAP_TYPE_DEFAULT;
					props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
					props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

					D3D12_RESOURCE_DESC res_desc = { 0 };
					res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
					res_desc.Width = win_width;
					res_desc.Height = win_height;
					res_desc.DepthOrArraySize = 1;
					res_desc.MipLevels = 1;
					res_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					res_desc.SampleDesc.Count = 1;
					res_desc.SampleDesc.Quality = 0;
					res_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
					res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

					CHECK_ERR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
						&res_desc, D3D12_RESOURCE_STATE_COPY_SOURCE,
						nullptr, IID_PPV_ARGS(&rt_output_img)));

					// Update the descriptor heap to the resized texture
					D3D12_CPU_DESCRIPTOR_HANDLE heap_handle = rt_shader_res_heap->GetCPUDescriptorHandleForHeapStart();

					D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = { 0 };
					uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
					device->CreateUnorderedAccessView(rt_output_img.Get(), nullptr, &uav_desc, heap_handle);
				}
			}
		}

		// Build the command list to clear the frame color
		CHECK_ERR(cmd_allocator->Reset());

		CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), nullptr));

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

		// Ray trace it!
		// Bind our descriptor heap with the output texture and the accel. structure
		std::vector<ID3D12DescriptorHeap*> heaps = { rt_shader_res_heap.Get() };
		cmd_list->SetDescriptorHeaps(heaps.size(), heaps.data());

		// Transition the output buffer back to a unordered access, since we'll have it copy source
		// from blitting the image
		{
			D3D12_RESOURCE_BARRIER res_barrier;
			res_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			res_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			res_barrier.Transition.pResource = rt_output_img.Get();
			res_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
			res_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			res_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			cmd_list->ResourceBarrier(1, &res_barrier);
		}

		D3D12_DISPATCH_RAYS_DESC dispatch_rays = { 0 };
		// Tell the dispatch rays how we built our shader binding table

		// RayGen is first, and has a shader identifier and one param
		dispatch_rays.RayGenerationShaderRecord.StartAddress = rt_shader_table->GetGPUVirtualAddress();
		dispatch_rays.RayGenerationShaderRecord.SizeInBytes = 2 * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
		
		// Miss is next, followed by hit, each is just a shader identifier
		dispatch_rays.MissShaderTable.StartAddress =
			rt_shader_table->GetGPUVirtualAddress() + 2 * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
		dispatch_rays.MissShaderTable.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
		dispatch_rays.MissShaderTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

		dispatch_rays.HitGroupTable.StartAddress =
			rt_shader_table->GetGPUVirtualAddress() + 4 * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
		dispatch_rays.HitGroupTable.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
		dispatch_rays.HitGroupTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

		dispatch_rays.Width = win_width;
		dispatch_rays.Height = win_height;
		dispatch_rays.Depth = 1;

		cmd_list->SetDescriptorHeaps(1, rt_shader_res_heap.GetAddressOf());
		cmd_list->SetPipelineState1(rt_state_object.Get());

		cmd_list->DispatchRays(&dispatch_rays);

		// Now copy the output buffer data to the back buffer to display it
		// Transition the raytrace image to copy source and the back buffer to copy dest
		{
			std::array<D3D12_RESOURCE_BARRIER, 2> barriers;
			barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			barriers[0].Transition.pResource = rt_output_img.Get();
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

			barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			barriers[1].Transition.pResource = render_targets[back_buffer_idx].Get();
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			
			cmd_list->ResourceBarrier(barriers.size(), barriers.data());
		}

		cmd_list->CopyResource(render_targets[back_buffer_idx].Get(), rt_output_img.Get());

		// Swap the back buffer to present so it can be displayed
		{
			D3D12_RESOURCE_BARRIER res_barrier;
			res_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			res_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			res_barrier.Transition.pResource = render_targets[back_buffer_idx].Get();
			res_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
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

		// TODO: Seems like sometimes this fence doesn't really wait how I expect it to
		// and I get flickering?
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

// Pretty-print a state object tree.
void PrintStateObjectDesc(const D3D12_STATE_OBJECT_DESC* desc) {
	std::wstringstream wstr;
	wstr << L"\n";
	wstr << L"--------------------------------------------------------------------\n";
	wstr << L"| D3D12 State Object 0x" << static_cast<const void*>(desc) << L": ";
	if (desc->Type == D3D12_STATE_OBJECT_TYPE_COLLECTION) wstr << L"Collection\n";
	if (desc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) wstr << L"Raytracing Pipeline\n";

	auto ExportTree = [](UINT depth, UINT numExports, const D3D12_EXPORT_DESC* exports)
	{
		std::wostringstream woss;
		for (UINT i = 0; i < numExports; i++)
		{
			woss << L"|";
			if (depth > 0)
			{
				for (UINT j = 0; j < 2 * depth - 1; j++) woss << L" ";
			}
			woss << L" [" << i << L"]: ";
			if (exports[i].ExportToRename) woss << exports[i].ExportToRename << L" --> ";
			woss << exports[i].Name << L"\n";
		}
		return woss.str();
	};

	for (UINT i = 0; i < desc->NumSubobjects; i++)
	{
		wstr << L"| [" << i << L"]: ";
		switch (desc->pSubobjects[i].Type)
		{
		case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
			wstr << L"Global Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
			break;
		case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
			wstr << L"Local Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
			break;
		case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
			wstr << L"Node Mask: 0x" << std::hex << std::setfill(L'0') << std::setw(8) << *static_cast<const UINT*>(desc->pSubobjects[i].pDesc) << std::setw(0) << std::dec << L"\n";
			break;
		case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
		{
			wstr << L"DXIL Library 0x";
			auto lib = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(desc->pSubobjects[i].pDesc);
			wstr << lib->DXILLibrary.pShaderBytecode << L", " << lib->DXILLibrary.BytecodeLength << L" bytes\n";
			wstr << ExportTree(1, lib->NumExports, lib->pExports);
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:
		{
			wstr << L"Existing Library 0x";
			auto collection = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(desc->pSubobjects[i].pDesc);
			wstr << collection->pExistingCollection << L"\n";
			wstr << ExportTree(1, collection->NumExports, collection->pExports);
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
		{
			wstr << L"Subobject to Exports Association (Subobject [";
			auto association = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(desc->pSubobjects[i].pDesc);
			UINT index = static_cast<UINT>(association->pSubobjectToAssociate - desc->pSubobjects);
			wstr << index << L"])\n";
			for (UINT j = 0; j < association->NumExports; j++)
			{
				wstr << L"|  [" << j << L"]: " << association->pExports[j] << L"\n";
			}
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
		{
			wstr << L"DXIL Subobjects to Exports Association (";
			auto association = static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(desc->pSubobjects[i].pDesc);
			wstr << association->SubobjectToAssociate << L")\n";
			for (UINT j = 0; j < association->NumExports; j++)
			{
				wstr << L"|  [" << j << L"]: " << association->pExports[j] << L"\n";
			}
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
		{
			wstr << L"Raytracing Shader Config\n";
			auto config = static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(desc->pSubobjects[i].pDesc);
			wstr << L"|  [0]: Max Payload Size: " << config->MaxPayloadSizeInBytes << L" bytes\n";
			wstr << L"|  [1]: Max Attribute Size: " << config->MaxAttributeSizeInBytes << L" bytes\n";
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
		{
			wstr << L"Raytracing Pipeline Config\n";
			auto config = static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(desc->pSubobjects[i].pDesc);
			wstr << L"|  [0]: Max Recursion Depth: " << config->MaxTraceRecursionDepth << L"\n";
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
		{
			wstr << L"Hit Group (";
			auto hitGroup = static_cast<const D3D12_HIT_GROUP_DESC*>(desc->pSubobjects[i].pDesc);
			wstr << (hitGroup->HitGroupExport ? hitGroup->HitGroupExport : L"[none]") << L")\n";
			wstr << L"|  [0]: Any Hit Import: " << (hitGroup->AnyHitShaderImport ? hitGroup->AnyHitShaderImport : L"[none]") << L"\n";
			wstr << L"|  [1]: Closest Hit Import: " << (hitGroup->ClosestHitShaderImport ? hitGroup->ClosestHitShaderImport : L"[none]") << L"\n";
			wstr << L"|  [2]: Intersection Import: " << (hitGroup->IntersectionShaderImport ? hitGroup->IntersectionShaderImport : L"[none]") << L"\n";
			break;
		}
		}
		wstr << L"|--------------------------------------------------------------------\n";
	}
	wstr << L"\n";
	std::wcout << wstr.str() << std::endl << std::flush;
	OutputDebugStringW(wstr.str().c_str());
}