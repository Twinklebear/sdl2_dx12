cmake_minimum_required(VERSION 3.5)
project(sdl2_dx12)

add_definitions(-DNOMINMAX -DSDL_MAIN_HANDLED)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_LIST_DIR}/cmake")

find_package(Threads REQUIRED)
find_package(SDL2 REQUIRED)
find_package(D3D12 REQUIRED)

add_subdirectory(res)

add_dxil_embed_library(rt_shader res/rt_shader.hlsl
	# TODO WILL Do we need the default linkage arg?
	COMPILE_OPTIONS -Zi -default-linkage external)

add_executable(sdl2_dx12 main.cpp)

set_target_properties(sdl2_dx12 PROPERTIES
	CXX_STANDARD 14
	CXX_STANDARD_REQUIRED ON)

target_include_directories(sdl2_dx12 PUBLIC
	$<BUILD_INTERFACE:${SDL2_INCLUDE_DIR}>
	$<BUILD_INTERFACE:${D3D12_INCLUDE_PATH}>)

target_link_libraries(sdl2_dx12 PUBLIC
	rt_shader ${SDL2_LIBRARY} ${D3D12_LIBRARIES})

install(TARGETS sdl2_dx12 DESTINATION bin)

