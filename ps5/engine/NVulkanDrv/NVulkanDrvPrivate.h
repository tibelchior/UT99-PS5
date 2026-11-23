/*------------------------------------------------------------------------------------
	NVulkanDrv: Vulkan render device for the UT99 (ut99dc) engine.
	A port of NOpenGLESDrv with the same draw semantics; frames are rendered offscreen
	and copied into the viewport's screen pointer (the SDL layer presents them).
------------------------------------------------------------------------------------*/

// STL and Vulkan first: the engine headers define macros (check, verify, clock...) that clash with them.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <map>
#include <array>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "RenderPrivate.h"

enum EVkShaderFlags : DWORD
{
	SF_Texture0  = 1 << 0,
	SF_Texture1  = 1 << 1,
	SF_Texture2  = 1 << 2,
	SF_Texture3  = 1 << 3,
	SF_VtxColor  = 1 << 4,
	SF_AlphaTest = 1 << 5,
	SF_Lightmap  = 1 << 6,
	SF_Fogmap    = 1 << 7,
	SF_Detail    = 1 << 8,
	SF_VtxFog    = 1 << 9,
	SF_ModulatedDecalMask = 1 << 10,
};

enum EVkAttrib { AT_Position, AT_TexCoord0, AT_TexCoord1, AT_TexCoord2, AT_TexCoord3, AT_VtxColor, AT_VtxFog, AT_Count };

enum EVkBlend { BM_Opaque, BM_Translucent, BM_Modulated, BM_Highlighted };

class DLL_EXPORT UNVulkanRenderDevice : public URenderDevice
{
	DECLARE_CLASS(UNVulkanRenderDevice, URenderDevice, CLASS_Config)

	static constexpr INT MaxTexUnits = 4;

	// Options.
	UBOOL NoFiltering;
	UBOOL Overbright;
	UBOOL DetailTextures;

	// Textures.
	struct FVkTexture
	{
		VkImage Image;
		VkImageView View;
		INT Levels;
		INT Width, Height;
		VkFormat Format;
		INT Sampler;       // index into Samplers
	};
	std::vector<FVkTexture> Textures;   // index+1 is the texture id used in descriptor keys
	TMap<QWORD, INT> BindMap;           // cache id -> index into Textures

	struct FTexInfo
	{
		QWORD CurrentCacheID;
		FLOAT UMult, VMult, UPan, VPan;
	} TexInfo[MaxTexUnits];

	// Vertex staging (CPU side, as in the GLES driver).
	FLOAT* VtxData;
	FLOAT* VtxDataEnd;
	FLOAT* VtxDataPtr;
	DWORD VtxDataSize;
	DWORD VtxPolyVerts;
	_WORD* IdxData;
	_WORD* IdxDataEnd;
	_WORD* IdxDataPtr;
	DWORD IdxDataSize;
	_WORD IdxCount;
	_WORD IdxBase;

	// Logical state.
	DWORD CurrentShaderFlags;   // what the next draw wants
	DWORD BoundShaderFlags;     // what the pending vertices were written for
	UBOOL ShaderBound;
	DWORD CurrentPolyFlags;
	FLOAT CurrentBrightness;
	FLOAT RProjZ, Aspect, RFX2, RFY2;
	FLOAT MtxMVP[16];           // pending (column major, Vulkan clip space)
	FLOAT AppliedMVP[16];       // what draws use
	FLOAT AppliedBrightness;
	UBOOL PushDirty;
	FPlane ColorMod;

	// Applied pipeline state (changed only after a flush).
	INT BlendMode;
	UBOOL DepthWrite, DepthTest, ColorWrite;
	INT VpX, VpY, VpW, VpH;     // viewport, top-left origin
	UBOOL VpDirty;

	struct FCachedSceneNode
	{
		FLOAT FovAngle, FX, FY;
		INT X, Y, XB, YB, SizeX, SizeY;
	} CurrentSceneNode;

	// Vulkan objects (opaque to the header; see the .cpp).
	struct FVk* Vk;

	UNVulkanRenderDevice();
	static void InternalClassInitializer( UClass* Class );
	void StaticConstructor();

	// URenderDevice interface.
	virtual UBOOL Init( UViewport* InViewport, INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen ) override;
	virtual UBOOL SetRes( INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen ) override;
	virtual void Exit() override;
	virtual void Flush( UBOOL AllowPrecache ) override;
	virtual UBOOL Exec( const TCHAR* Cmd, FOutputDevice& Ar ) override;
	virtual void Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* InHitData, INT* InHitSize ) override;
	virtual void Unlock( UBOOL Blit ) override;
	virtual void DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet ) override;
	virtual void DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Texture, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* SpanBuffer ) override;
	virtual void DrawTile( FSceneNode* Frame, FTextureInfo& Texture, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Light, FPlane Fog, DWORD PolyFlags ) override;
	virtual void EndFlash() override;
	virtual void GetStats( TCHAR* Result ) override;
	virtual void Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 ) override;
	virtual void Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, FLOAT Z ) override;
	virtual void PushHit( const BYTE* Data, INT Count ) override;
	virtual void PopHit( INT Count, UBOOL bForce ) override;
	virtual void ReadPixels( FColor* Pixels ) override;
	virtual void ClearZ( FSceneNode* Frame ) override;

	// Internals.
	void FlushTriangles();
	void EnsureSpace( INT NumPts );
	void SetShader( DWORD ShaderFlags );
	void SetSceneNode( FSceneNode* Frame );
	void SetBlend( DWORD PolyFlags );
	void SetTexture( INT TMU, FTextureInfo& Info, DWORD PolyFlags, FLOAT PanBias );
	void ResetTexture( INT TMU );
	INT  UploadTexture( FTextureInfo& Info, UBOOL Masked, INT ExistingIndex );

	inline void BeginPoly() { VtxPolyVerts = 0; IdxBase = IdxCount; }
	inline void AttribFloat2( FLOAT X, FLOAT Y ) { *VtxDataPtr++ = X; *VtxDataPtr++ = Y; }
	inline void AttribFloat3( FLOAT X, FLOAT Y, FLOAT Z ) { *VtxDataPtr++ = X; *VtxDataPtr++ = Y; *VtxDataPtr++ = Z; }
	inline void AttribFloat3( const FLOAT* V ) { *VtxDataPtr++ = V[0]; *VtxDataPtr++ = V[1]; *VtxDataPtr++ = V[2]; }
	inline void AttribFloat4( FLOAT X, FLOAT Y, FLOAT Z, FLOAT W ) { *VtxDataPtr++ = X; *VtxDataPtr++ = Y; *VtxDataPtr++ = Z; *VtxDataPtr++ = W; }
	inline void AttribFloat4( const FLOAT* V ) { *VtxDataPtr++ = V[0]; *VtxDataPtr++ = V[1]; *VtxDataPtr++ = V[2]; *VtxDataPtr++ = V[3]; }
	inline void PolyVertex() { ++VtxPolyVerts; }
	inline void EndPoly()
	{
		*IdxDataPtr++ = IdxCount++;
		*IdxDataPtr++ = IdxCount++;
		*IdxDataPtr++ = IdxCount++;
		for( DWORD i = 3; i < VtxPolyVerts; ++i )
		{
			*IdxDataPtr++ = IdxBase;
			*IdxDataPtr++ = IdxCount - 1;
			*IdxDataPtr++ = IdxCount++;
		}
	}
};
