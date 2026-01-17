#pragma once

// Simple vertex and pixel shaders for OSD text rendering
// Compiled with fxc.exe /T vs_4_0 /E VSMain and /T ps_4_0 /E PSMain

// Vertex Shader (vs_4_0)
// Input: float2 pos, float2 uv
// Output: float4 pos, float2 uv
static const unsigned char osd_vs_bytecode[] = {
	// DXBC bytecode for simple passthrough VS
	// cbuffer Constants : register(b0) { float4 screenSize; }
	// Takes screen-space coords and converts to clip space
	0x44,
	0x58,
	0x42,
	0x43, // DXBC magic
	      // ... (placeholder - we'll use inline HLSL compilation instead)
};

// For simplicity, we'll compile shaders at runtime using D3DCompile
// This avoids needing pre-compiled bytecode

static const char *osd_vs_hlsl = "cbuffer Constants : register(b0) {\n"
				 "    float2 screenSize;\n"
				 "    float2 padding;\n"
				 "};\n"
				 "struct VS_INPUT {\n"
				 "    float2 pos : POSITION;\n"
				 "    float2 uv : TEXCOORD0;\n"
				 "};\n"
				 "struct VS_OUTPUT {\n"
				 "    float4 pos : SV_POSITION;\n"
				 "    float2 uv : TEXCOORD0;\n"
				 "};\n"
				 "VS_OUTPUT VSMain(VS_INPUT input) {\n"
				 "    VS_OUTPUT output;\n"
				 "    // Convert from screen pixels to clip space (-1 to 1)\n"
				 "    output.pos.x = (input.pos.x / screenSize.x) * 2.0 - 1.0;\n"
				 "    output.pos.y = 1.0 - (input.pos.y / screenSize.y) * 2.0;\n"
				 "    output.pos.z = 0.0;\n"
				 "    output.pos.w = 1.0;\n"
				 "    output.uv = input.uv;\n"
				 "    return output;\n"
				 "}\n";

static const char *osd_ps_hlsl = "Texture2D fontTexture : register(t0);\n"
				 "SamplerState fontSampler : register(s0);\n"
				 "cbuffer ColorConstants : register(b1) {\n"
				 "    float4 textColor;\n"
				 "};\n"
				 "struct PS_INPUT {\n"
				 "    float4 pos : SV_POSITION;\n"
				 "    float2 uv : TEXCOORD0;\n"
				 "};\n"
				 "float4 PSMain(PS_INPUT input) : SV_TARGET {\n"
				 "    float alpha = fontTexture.Sample(fontSampler, input.uv).r;\n"
				 "    return float4(textColor.rgb, textColor.a * alpha);\n"
				 "}\n";
