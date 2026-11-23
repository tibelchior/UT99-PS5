#version 450
// UT99 Vulkan renderer: vertex shader. One shader, specialized per feature set (FLAGS), mirroring NOpenGLESDrv.
layout(constant_id = 0) const uint FLAGS = 0u;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aTexCoord0;
layout(location = 2) in vec2 aTexCoord1;
layout(location = 3) in vec2 aTexCoord2;
layout(location = 4) in vec2 aTexCoord3;
layout(location = 5) in vec4 aVtxColor;
layout(location = 6) in vec4 aVtxFog;

layout(location = 0) out vec3 vPosition;
layout(location = 1) out vec2 vTexCoord0;
layout(location = 2) out vec2 vTexCoord1;
layout(location = 3) out vec2 vTexCoord2;
layout(location = 4) out vec2 vTexCoord3;
layout(location = 5) out vec4 vVtxColor;
layout(location = 6) out vec4 vVtxFog;

layout(push_constant) uniform PC { mat4 uMtx; float uBrightness; } pc;

void main()
{
	vPosition = aPosition;
	vTexCoord0 = vec2(0.0); vTexCoord1 = vec2(0.0); vTexCoord2 = vec2(0.0); vTexCoord3 = vec2(0.0);
	vVtxColor = vec4(1.0); vVtxFog = vec4(0.0);
	if ((FLAGS & 1u) != 0u)   vTexCoord0 = aTexCoord0;
	if ((FLAGS & 2u) != 0u)   vTexCoord1 = aTexCoord1;
	if ((FLAGS & 4u) != 0u)   vTexCoord2 = aTexCoord2;
	if ((FLAGS & 8u) != 0u)   vTexCoord3 = aTexCoord3;
	if ((FLAGS & 16u) != 0u)  vVtxColor = aVtxColor;
	if ((FLAGS & 512u) != 0u) vVtxFog = aVtxFog;
	gl_Position = pc.uMtx * vec4(aPosition, 1.0);
}
