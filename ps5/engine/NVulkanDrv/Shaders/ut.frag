#version 450
// UT99 Vulkan renderer: fragment shader, a port of NOpenGLESDrv's FragmentShader.glsl.inc.
layout(constant_id = 0) const uint FLAGS = 0u;
layout(constant_id = 1) const float LIGHTMAP_OVERBRIGHT = 1.0;

layout(location = 0) in vec3 vPosition;
layout(location = 1) in vec2 vTexCoord0;
layout(location = 2) in vec2 vTexCoord1;
layout(location = 3) in vec2 vTexCoord2;
layout(location = 4) in vec2 vTexCoord3;
layout(location = 5) in vec4 vVtxColor;
layout(location = 6) in vec4 vVtxFog;

layout(set = 0, binding = 0) uniform sampler2D uTexture0;
layout(set = 0, binding = 1) uniform sampler2D uTexture1;
layout(set = 0, binding = 2) uniform sampler2D uTexture2;
layout(set = 0, binding = 3) uniform sampler2D uTexture3;

layout(push_constant) uniform PC { mat4 uMtx; float uBrightness; } pc;

layout(location = 0) out vec4 outColor;

bool F(uint bit) { return (FLAGS & bit) != 0u; }

void main()
{
	vec4 Out = vec4(1.0);

	if (F(1u)) Out = texture(uTexture0, vTexCoord0);

	if (F(32u) && Out.a < 0.5) discard;                       // SF_AlphaTest

	if (F(1024u)) {                                            // SF_ModulatedDecalMask
		float Luma = max(max(Out.r, Out.g), Out.b);
		if (Luma > 0.42) discard;
	}

	if (F(2u)) {
		vec4 Texel1 = texture(uTexture1, vTexCoord1);
		if (F(64u)) Out.rgb *= Texel1.rgb * 2.0 * LIGHTMAP_OVERBRIGHT;   // SF_Lightmap
		else        Out *= Texel1;
	}

	if (F(4u)) {
		vec4 Texel2 = texture(uTexture2, vTexCoord2);
		if (F(128u)) {                                         // SF_Fogmap
			Texel2 *= 2.0;
			Out.rgb = Texel2.rgb + Out.rgb * (1.0 - Texel2.a);
			Out.a = Texel2.a;
		} else {
			Out *= Texel2;
		}
	}

	if (F(8u)) {
		vec4 Texel3 = texture(uTexture3, vTexCoord3) * 2.0;
		if (F(256u)) {                                         // SF_Detail
			float DetailScale = clamp(0.65 - vPosition.z / 380.0, 0.0, 1.0);
			Texel3 = mix(vec4(1.0), Texel3, DetailScale);
		}
		Out.rgb *= Texel3.rgb;
	}

	if (F(16u)) Out *= vVtxColor;                              // SF_VtxColor

	if (F(512u)) Out.rgb = vVtxFog.rgb + Out.rgb * (1.0 - vVtxFog.a);   // SF_VtxFog

	Out.rgb *= 0.5 + pc.uBrightness;

	outColor = Out;
}
