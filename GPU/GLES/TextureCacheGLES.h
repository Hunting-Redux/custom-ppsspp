// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <map>

#include "gfx_es2/gpu_features.h"
#include "gfx/gl_common.h"
#include "thin3d/GLRenderManager.h"
#include "GPU/GPUInterface.h"
#include "GPU/GPUState.h"
#include "GPU/GLES/TextureScalerGLES.h"
#include "GPU/Common/TextureCacheCommon.h"

struct VirtualFramebuffer;
class FramebufferManagerGLES;
class DepalShaderCacheGLES;
class ShaderManagerGLES;
class DrawEngineGLES;
class GLRTexture;

class TextureCacheGLES : public TextureCacheCommon {
public:
	TextureCacheGLES(Draw::DrawContext *draw);
	~TextureCacheGLES();

	void Clear(bool delete_them) override;
	void StartFrame();

	void SetFramebufferManager(FramebufferManagerGLES *fbManager);
	void SetDepalShaderCache(DepalShaderCacheGLES *dpCache) {
		depalShaderCache_ = dpCache;
	}
	void SetShaderManager(ShaderManagerGLES *sm) {
		shaderManager_ = sm;
	}
	void SetDrawEngine(DrawEngineGLES *td) {
		drawEngine_ = td;
	}

	void ForgetLastTexture() override {
		lastBoundTexture = nullptr;
		gstate_c.Dirty(DIRTY_TEXTURE_PARAMS);
	}
	void InvalidateLastTexture(TexCacheEntry *entry = nullptr) override {
		if (!entry || entry->textureName == lastBoundTexture) {
			lastBoundTexture = nullptr;
		}
	}

	void SetFramebufferSamplingParams(u16 bufferWidth, u16 bufferHeight, bool forcePoint);
	bool GetCurrentTextureDebug(GPUDebugBuffer &buffer, int level) override;

	void DeviceLost();
	void DeviceRestore(Draw::DrawContext *draw);

protected:
	void BindTexture(TexCacheEntry *entry) override;
	void Unbind() override;
	void ReleaseTexture(TexCacheEntry *entry, bool delete_them) override;

private:
	void UpdateSamplingParams(TexCacheEntry &entry, bool force);
	void LoadTextureLevel(TexCacheEntry &entry, ReplacedTexture &replaced, int level, int scaleFactor, Draw::DataFormat dstFmt);
	Draw::DataFormat GetDestFormat(GETextureFormat format, GEPaletteFormat clutFormat) const;

	TexCacheEntry::TexStatus CheckAlpha(const uint8_t *pixelData, Draw::DataFormat dstFmt, int stride, int w, int h);
	void UpdateCurrentClut(GEPaletteFormat clutFormat, u32 clutBase, bool clutIndexIsSimple) override;
	void ApplyTextureFramebuffer(TexCacheEntry *entry, VirtualFramebuffer *framebuffer) override;

	void BuildTexture(TexCacheEntry *const entry) override;

	GLRenderManager *render_;

	TextureScalerGLES scaler;

	GLRTexture *lastBoundTexture;

	FramebufferManagerGLES *framebufferManagerGL_;
	DepalShaderCacheGLES *depalShaderCache_;
	ShaderManagerGLES *shaderManager_;
	DrawEngineGLES *drawEngine_;

	GLRInputLayout *shadeInputLayout_ = nullptr;

	enum { INVALID_TEX = -1 };
};

Draw::DataFormat getClutDestFormat(GEPaletteFormat format);
