// https://developer.nvidia.com/rtx/raytracing/dxr/DX12-Raytracing-tutorial-Part-2
struct HitInfo {
  float4 color_dist;
};

// Attributes output by the raytracing when hitting a surface,
// here the barycentric coordinates
struct Attributes {
  float2 bary;
};

// Raytracing output texture, accessed as a UAV
RWTexture2D<float4> output : register(u0);

// Raytracing acceleration structure, accessed as a SRV
RaytracingAccelerationStructure scene : register(t0);

[shader("raygeneration")] 
void RayGen() {
  // Initialize the ray payload
  HitInfo payload;
  payload.color_dist = float4(0.9, 0.6, 0.2, 1);

  // Get the location within the dispatched 2D grid of work items
  // (often maps to pixels, so this could represent a pixel coordinate).
  uint2 pixel = DispatchRaysIndex().xy;

  output[pixel] = float4(payload.color_dist.rgb, 1.f);
}

[shader("miss")]
void Miss(inout HitInfo payload : SV_RayPayload) {
    payload.color_dist = float4(0.2f, 0.2f, 0.8f, -1.f);
}

[shader("closesthit")] 
void ClosestHit(inout HitInfo payload, Attributes attrib) {
  payload.color_dist = float4(1, 1, 0, RayTCurrent());
}

