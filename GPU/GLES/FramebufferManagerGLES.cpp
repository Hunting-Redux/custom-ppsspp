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

#include <set>
#include <algorithm>

#include "profiler/profiler.h"
#include "gfx/gl_common.h"
#include "gfx/gl_debug_log.h"
#include "gfx_es2/glsl_program.h"
#include "thin3d/thin3d.h"

#include "base/timeutil.h"
#include "file/vfs.h"
#include "math/lin/matrix4x4.h"

#include "Common/ColorConv.h"
#include "Core/Host.h"
#include "Core/MemMap.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"
#include "Core/Reporting.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"

#include "GPU/Common/PostShader.h"
#include "GPU/Common/ShaderTranslation.h"
#include "GPU/Common/TextureDecoder.h"
#include "GPU/Common/FramebufferCommon.h"
#include "GPU/Debugger/Stepping.h"
#include "GPU/GLES/FramebufferManagerGLES.h"
#include "GPU/GLES/TextureCacheGLES.h"
#include "GPU/GLES/DrawEngineGLES.h"
#include "GPU/GLES/ShaderManagerGLES.h"

static const char tex_fs[] = R"(
#if __VERSION__ >= 130
#define varying in
#define texture2D texture
#define gl_FragColor fragColor0
out vec4 fragColor0;
#endif
#ifdef GL_ES
precision mediump float;
#endif
uniform sampler2D sampler0;
varying vec2 v_texcoord0;
void main() {
	gl_FragColor = texture2D(sampler0, v_texcoord0);
}
)";

static const char basic_vs[] = R"(
#if __VERSION__ >= 130
#define attribute in
#define varying out
#endif
attribute vec4 a_position;
attribute vec2 a_texcoord0;
varying vec2 v_texcoord0;
void main() {
  v_texcoord0 = a_texcoord0;
  gl_Position = a_position;
}
)";

void FramebufferManagerGLES::CompileDraw2DProgram() {
	if (!draw2dprogram_) {
		std::string errorString;
		static std::string vs_code, fs_code;
		vs_code = ApplyGLSLPrelude(basic_vs, GL_VERTEX_SHADER);
		fs_code = ApplyGLSLPrelude(tex_fs, GL_FRAGMENT_SHADER);
		std::vector<GLRShader *> shaders;
		shaders.push_back(render_->CreateShader(GL_VERTEX_SHADER, vs_code, "draw2d"));
		shaders.push_back(render_->CreateShader(GL_FRAGMENT_SHADER, fs_code, "draw2d"));

		std::vector<GLRProgram::UniformLocQuery> queries;
		queries.push_back({ &u_draw2d_tex, "u_tex" });
		std::vector<GLRProgram::Initializer> initializers;
		initializers.push_back({ &u_draw2d_tex, 0 });
		std::vector<GLRProgram::Semantic> semantics;
		semantics.push_back({ 0, "a_position" });
		semantics.push_back({ 1, "a_texcoord0" });
		draw2dprogram_ = render_->CreateProgram(shaders, semantics, queries, initializers, false);
		for (auto shader : shaders)
			render_->DeleteShader(shader);
		CompilePostShader();
	}
}

void FramebufferManagerGLES::CompilePostShader() {
	SetNumExtraFBOs(0);
	const ShaderInfo *shaderInfo = 0;
	if (g_Config.sPostShaderName != "Off") {
		ReloadAllPostShaderInfo();
		shaderInfo = GetPostShaderInfo(g_Config.sPostShaderName);
	}

	if (shaderInfo) {
		std::string errorString;
		postShaderAtOutputResolution_ = shaderInfo->outputResolution;

		size_t sz;
		char *vs = (char *)VFSReadFile(shaderInfo->vertexShaderFile.c_str(), &sz);
		if (!vs)
			return;
		char *fs = (char *)VFSReadFile(shaderInfo->fragmentShaderFile.c_str(), &sz);
		if (!fs) {
			free(vs);
			return;
		}

		std::string vshader;
		std::string fshader;
		bool translationFailed = false;
		if (gl_extensions.IsCoreContext) {
			// Gonna have to upconvert the shaders.
			if (!TranslateShader(&vshader, GLSL_300, nullptr, vs, GLSL_140, Draw::ShaderStage::VERTEX, &errorString)) {
				translationFailed = true;
				ELOG("Failed to translate post-vshader: %s", errorString.c_str());
			}
			if (!TranslateShader(&fshader, GLSL_300, nullptr, fs, GLSL_140, Draw::ShaderStage::FRAGMENT, &errorString)) {
				translationFailed = true;
				ELOG("Failed to translate post-fshader: %s", errorString.c_str());
			}
		} else {
			vshader = vs;
			fshader = fs;
		}

		if (!translationFailed) {
			SetNumExtraFBOs(1);

			std::vector<GLRShader *> shaders;
			shaders.push_back(render_->CreateShader(GL_VERTEX_SHADER, vshader, "postshader"));
			shaders.push_back(render_->CreateShader(GL_FRAGMENT_SHADER, fshader, "postshader"));
			std::vector<GLRProgram::UniformLocQuery> queries;
			queries.push_back({ &u_postShaderTex, "tex" });
			queries.push_back({ &deltaLoc_, "u_texelDelta" });
			queries.push_back({ &pixelDeltaLoc_, "u_pixelDelta" });
			queries.push_back({ &timeLoc_, "u_time" });
			queries.push_back({ &videoLoc_, "u_video" });

			std::vector<GLRProgram::Initializer> inits;
			inits.push_back({ &u_postShaderTex, 0, 0 });
			std::vector<GLRProgram::Semantic> semantics;
			semantics.push_back({ 0, "a_position" });
			semantics.push_back({ 1, "a_texcoord0" });
			postShaderProgram_ = render_->CreateProgram(shaders, semantics, queries, inits, false);
			postShaderModules_ = shaders;
		} else {
			ERROR_LOG(FRAMEBUF, "Failed to translate post shader!");
		}
		free(vs);
		free(fs);

		if (!postShaderProgram_) {
			// DO NOT turn this into a report, as it will pollute our logs with all kinds of
			// user shader experiments.
			ERROR_LOG(FRAMEBUF, "Failed to build post-processing program from %s and %s!\n%s", shaderInfo->vertexShaderFile.c_str(), shaderInfo->fragmentShaderFile.c_str(), errorString.c_str());
			ShowPostShaderError(errorString);
			usePostShader_ = false;
		} else {
			usePostShader_ = true;
		}
	} else {
		postShaderProgram_ = nullptr;
		usePostShader_ = false;
	}
}

void FramebufferManagerGLES::ShowPostShaderError(const std::string &errorString) {
	// let's show the first line of the error string as an OSM.
	std::set<std::string> blacklistedLines;
	// These aren't useful to show, skip to the first interesting line.
	blacklistedLines.insert("Fragment shader failed to compile with the following errors:");
	blacklistedLines.insert("Vertex shader failed to compile with the following errors:");
	blacklistedLines.insert("Compile failed.");
	blacklistedLines.insert("");

	std::string firstLine;
	size_t start = 0;
	for (size_t i = 0; i < errorString.size(); i++) {
		if (errorString[i] == '\n' && i == start) {
			start = i + 1;
		} else if (errorString[i] == '\n') {
			firstLine = errorString.substr(start, i - start);
			if (blacklistedLines.find(firstLine) == blacklistedLines.end()) {
				break;
			}
			start = i + 1;
			firstLine.clear();
		}
	}
	if (!firstLine.empty()) {
		host->NotifyUserMessage("Post-shader error: " + firstLine + "...", 10.0f, 0xFF3090FF);
	} else {
		host->NotifyUserMessage("Post-shader error, see log for details", 10.0f, 0xFF3090FF);
	}
}

void FramebufferManagerGLES::Bind2DShader() {
	render_->BindProgram(draw2dprogram_);
}

void FramebufferManagerGLES::BindPostShader(const PostShaderUniforms &uniforms) {
	// Make sure we've compiled the shader.
	if (!postShaderProgram_) {
		CompileDraw2DProgram();
	}

	bool failed = false;
	std::string errorMessage;
	for (size_t i = 0; i < postShaderModules_.size(); ++i) {
		auto &shader = postShaderModules_[i];
		if (shader->failed) {
			failed = true;
			errorMessage += shader->error + "\n";
		}

		if (shader->valid || shader->failed) {
			render_->DeleteShader(shader);
			postShaderModules_.erase(postShaderModules_.begin() + i);
			// Check this index again.
			i--;
		}
	}

	if (failed) {
		ShowPostShaderError(errorMessage);
		// Show stuff if possible in an upcoming frame.
		usePostShader_ = false;
	}

	render_->BindProgram(postShaderProgram_);
	if (deltaLoc_ != -1)
		render_->SetUniformF(&deltaLoc_, 2, uniforms.texelDelta);
	if (pixelDeltaLoc_ != -1)
		render_->SetUniformF(&pixelDeltaLoc_, 2, uniforms.pixelDelta);
	if (timeLoc_ != -1)
		render_->SetUniformF(&timeLoc_, 4, uniforms.time);
	if (videoLoc_ != -1)
		render_->SetUniformF(&videoLoc_, 1, &uniforms.video);
}

FramebufferManagerGLES::FramebufferManagerGLES(Draw::DrawContext *draw, GLRenderManager *render) :
	FramebufferManagerCommon(draw),
	render_(render)
{
	needBackBufferYSwap_ = true;
	needGLESRebinds_ = true;
	CreateDeviceObjects();
	render_ = (GLRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
}

void FramebufferManagerGLES::Init() {
	FramebufferManagerCommon::Init();
	// Workaround for upscaling shaders where we force x1 resolution without saving it
	Resized();
	CompileDraw2DProgram();
}

void FramebufferManagerGLES::SetTextureCache(TextureCacheGLES *tc) {
	textureCacheGL_ = tc;
	textureCache_ = tc;
}

void FramebufferManagerGLES::SetShaderManager(ShaderManagerGLES *sm) {
	shaderManagerGL_ = sm;
	shaderManager_ = sm;
}

void FramebufferManagerGLES::SetDrawEngine(DrawEngineGLES *td) {
	drawEngineGL_ = td;
	drawEngine_ = td;
}

void FramebufferManagerGLES::CreateDeviceObjects() {
	CompileDraw2DProgram();

	std::vector<GLRInputLayout::Entry> entries;
	entries.push_back({ 0, 3, GL_FLOAT, GL_FALSE, sizeof(Simple2DVertex), offsetof(Simple2DVertex, pos) });
	entries.push_back({ 1, 2, GL_FLOAT, GL_FALSE, sizeof(Simple2DVertex), offsetof(Simple2DVertex, uv) });
	simple2DInputLayout_ = render_->CreateInputLayout(entries);
}

void FramebufferManagerGLES::DestroyDeviceObjects() {
	if (simple2DInputLayout_) {
		render_->DeleteInputLayout(simple2DInputLayout_);
		simple2DInputLayout_ = nullptr;
	}

	if (draw2dprogram_) {
		render_->DeleteProgram(draw2dprogram_);
		draw2dprogram_ = nullptr;
	}
	if (postShaderProgram_) {
		render_->DeleteProgram(postShaderProgram_);
		postShaderProgram_ = nullptr;
	}
	// Will usually be clear already.
	for (auto iter : postShaderModules_) {
		render_->DeleteShader(iter);
	}
	postShaderModules_.clear();
	if (drawPixelsTex_) {
		render_->DeleteTexture(drawPixelsTex_);
		drawPixelsTex_ = 0;
	}
	if (stencilUploadProgram_) {
		render_->DeleteProgram(stencilUploadProgram_);
		stencilUploadProgram_ = nullptr;
	}
	if (depthDownloadProgram_) {
		render_->DeleteProgram(depthDownloadProgram_);
		depthDownloadProgram_ = nullptr;
	}
}

FramebufferManagerGLES::~FramebufferManagerGLES() {
	DestroyDeviceObjects();

	delete [] convBuf_;
}

void FramebufferManagerGLES::MakePixelTexture(const u8 *srcPixels, GEBufferFormat srcPixelFormat, int srcStride, int width, int height, float &u1, float &v1) {
	if (drawPixelsTex_) {
		render_->DeleteTexture(drawPixelsTex_);
	}

	drawPixelsTex_ = render_->CreateTexture(GL_TEXTURE_2D);
	drawPixelsTexW_ = width;
	drawPixelsTexH_ = height;

	drawPixelsTexFormat_ = srcPixelFormat;

	// TODO: We can just change the texture format and flip some bits around instead of this.
	// Could share code with the texture cache perhaps.
	u32 neededSize = width * height * 4;
	u8 *convBuf = new u8[neededSize];
	for (int y = 0; y < height; y++) {
		const u16_le *src16 = (const u16_le *)srcPixels + srcStride * y;
		const u32_le *src32 = (const u32_le *)srcPixels + srcStride * y;
		u32 *dst = (u32 *)convBuf + width * y;
		switch (srcPixelFormat) {
		case GE_FORMAT_565:
			ConvertRGBA565ToRGBA8888((u32 *)dst, src16, width);
			break;

		case GE_FORMAT_5551:
			ConvertRGBA5551ToRGBA8888((u32 *)dst, src16, width);
			break;

		case GE_FORMAT_4444:
			ConvertRGBA4444ToRGBA8888((u32 *)dst, src16, width);
			break;

		case GE_FORMAT_8888:
			memcpy(dst, src32, 4 * width);
			break;

		case GE_FORMAT_INVALID:
			_dbg_assert_msg_(G3D, false, "Invalid pixelFormat passed to DrawPixels().");
			break;
		}
	}
	render_->TextureImage(drawPixelsTex_, 0, width, height, Draw::DataFormat::R8G8B8A8_UNORM, convBuf, GLRAllocType::NEW, false);
	render_->FinalizeTexture(drawPixelsTex_, 0, false);

	// TODO: Return instead?
	render_->BindTexture(TEX_SLOT_PSP_TEXTURE, drawPixelsTex_);
}

void FramebufferManagerGLES::SetViewport2D(int x, int y, int w, int h) {
	render_->SetViewport({ (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f });
}

// x, y, w, h are relative coordinates against destW/destH, which is not very intuitive.
// TODO: This could totally use fbo_blit in many cases.
void FramebufferManagerGLES::DrawActiveTexture(float x, float y, float w, float h, float destW, float destH, float u0, float v0, float u1, float v1, int uvRotation, int flags) {
	float texCoords[8] = {
		u0,v0,
		u1,v0,
		u1,v1,
		u0,v1,
	};

	static const GLushort indices[4] = { 0,1,3,2 };

	if (uvRotation != ROTATION_LOCKED_HORIZONTAL) {
		float temp[8];
		int rotation = 0;
		// Vertical and Vertical180 needed swapping after we changed the coordinate system.
		switch (uvRotation) {
		case ROTATION_LOCKED_HORIZONTAL180: rotation = 4; break;
		case ROTATION_LOCKED_VERTICAL: rotation = 6; break;
		case ROTATION_LOCKED_VERTICAL180: rotation = 2; break;
		}
		for (int i = 0; i < 8; i++) {
			temp[i] = texCoords[(i + rotation) & 7];
		}
		memcpy(texCoords, temp, sizeof(temp));
	}

	float pos[12] = {
		x,y,0,
		x + w,y,0,
		x + w,y + h,0,
		x,y + h,0
	};

	float invDestW = 1.0f / (destW * 0.5f);
	float invDestH = 1.0f / (destH * 0.5f);
	for (int i = 0; i < 4; i++) {
		pos[i * 3] = pos[i * 3] * invDestW - 1.0f;
		pos[i * 3 + 1] = pos[i * 3 + 1] * invDestH - 1.0f;
	}

	// We always want a plain state here, well, except for when it's used by the stencil stuff...
	render_->SetDepth(false, false, GL_ALWAYS);
	render_->SetRaster(false, GL_CCW, GL_FRONT, GL_FALSE);
	if (!(flags & DRAWTEX_KEEP_STENCIL_ALPHA)) {
		render_->SetNoBlendAndMask(0xF);
		render_->SetStencilDisabled();
	}

	// Upscaling postshaders don't look well with linear
	if (flags & DRAWTEX_LINEAR) {
		render_->SetTextureSampler(0, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_LINEAR, GL_LINEAR, 0.0f);
	} else {
		render_->SetTextureSampler(0, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_NEAREST, GL_NEAREST, 0.0f);
	}

	Simple2DVertex verts[4];
	memcpy(verts[0].pos, &pos[0], 12);
	memcpy(verts[1].pos, &pos[3], 12);
	memcpy(verts[3].pos, &pos[6], 12);
	memcpy(verts[2].pos, &pos[9], 12);
	memcpy(verts[0].uv, &texCoords[0], 8);
	memcpy(verts[1].uv, &texCoords[2], 8);
	memcpy(verts[3].uv, &texCoords[4], 8);
	memcpy(verts[2].uv, &texCoords[6], 8);

	uint32_t bindOffset;
	GLRBuffer *buffer;

	// Workaround: This might accidentally get called from ReportScreen screenshot-taking, and in that case
	// the push buffer is not mapped. This only happens if framebuffer blit support is not available.
	if (drawEngineGL_->GetPushVertexBuffer()->IsReady()) {
		void *dest = drawEngineGL_->GetPushVertexBuffer()->Push(sizeof(verts), &bindOffset, &buffer);
		memcpy(dest, verts, sizeof(verts));
		render_->BindVertexBuffer(simple2DInputLayout_, buffer, bindOffset);
		render_->Draw(GL_TRIANGLE_STRIP, 0, 4);
	}
}

void FramebufferManagerGLES::ReformatFramebufferFrom(VirtualFramebuffer *vfb, GEBufferFormat old) {
	if (!useBufferedRendering_ || !vfb->fbo) {
		return;
	}

	// Technically, we should at this point re-interpret the bytes of the old format to the new.
	// That might get tricky, and could cause unnecessary slowness in some games.
	// For now, we just clear alpha/stencil from 565, which fixes shadow issues in Kingdom Hearts.
	// (it uses 565 to write zeros to the buffer, then 4444 to actually render the shadow.)
	//
	// The best way to do this may ultimately be to create a new FBO (combine with any resize?)
	// and blit with a shader to that, then replace the FBO on vfb.  Stencil would still be complex
	// to exactly reproduce in 4444 and 8888 formats.

	if (old == GE_FORMAT_565) {
		// Clear alpha and stencil.
		draw_->BindFramebufferAsRenderTarget(vfb->fbo, { Draw::RPAction::KEEP, Draw::RPAction::KEEP, Draw::RPAction::CLEAR });
		render_->Clear(0, 0.0f, 0, GL_COLOR_BUFFER_BIT, 0x8, 0, 0, 0, 0);
	}
}

void FramebufferManagerGLES::BlitFramebufferDepth(VirtualFramebuffer *src, VirtualFramebuffer *dst) {
	bool matchingDepthBuffer = src->z_address == dst->z_address && src->z_stride != 0 && dst->z_stride != 0;
	bool matchingSize = src->width == dst->width && src->height == dst->height;

	// Note: we don't use CopyFramebufferImage here, because it would copy depth AND stencil.  See #9740.
	if (matchingDepthBuffer && matchingSize) {
		int w = std::min(src->renderWidth, dst->renderWidth);
		int h = std::min(src->renderHeight, dst->renderHeight);

		if (gstate_c.Supports(GPU_SUPPORTS_ARB_FRAMEBUFFER_BLIT | GPU_SUPPORTS_NV_FRAMEBUFFER_BLIT)) {
			// Let's only do this if not clearing depth.
			draw_->BlitFramebuffer(src->fbo, 0, 0, w, h, dst->fbo, 0, 0, w, h, Draw::FB_DEPTH_BIT, Draw::FB_BLIT_NEAREST);
			dst->last_frame_depth_updated = gpuStats.numFlips;
		}
	}
}

void FramebufferManagerGLES::BindFramebufferAsColorTexture(int stage, VirtualFramebuffer *framebuffer, int flags) {
	if (!framebuffer->fbo || !useBufferedRendering_) {
		render_->BindTexture(stage, nullptr);
		gstate_c.skipDrawReason |= SKIPDRAW_BAD_FB_TEXTURE;
		return;
	}

	// currentRenderVfb_ will always be set when this is called, except from the GE debugger.
	// Let's just not bother with the copy in that case.
	bool skipCopy = (flags & BINDFBCOLOR_MAY_COPY) == 0;
	if (GPUStepping::IsStepping()) {
		skipCopy = true;
	}
	if (!skipCopy && framebuffer == currentRenderVfb_) {
		// TODO: Maybe merge with bvfbs_?  Not sure if those could be packing, and they're created at a different size.
		Draw::Framebuffer *renderCopy = GetTempFBO(TempFBO::COPY, framebuffer->renderWidth, framebuffer->renderHeight, (Draw::FBColorDepth)framebuffer->colorDepth);
		if (renderCopy) {
			VirtualFramebuffer copyInfo = *framebuffer;
			copyInfo.fbo = renderCopy;

			CopyFramebufferForColorTexture(&copyInfo, framebuffer, flags);
			draw_->BindFramebufferAsTexture(renderCopy, stage, Draw::FB_COLOR_BIT, 0);
		} else {
			draw_->BindFramebufferAsTexture(framebuffer->fbo, stage, Draw::FB_COLOR_BIT, 0);
		}
	} else {
		draw_->BindFramebufferAsTexture(framebuffer->fbo, stage, Draw::FB_COLOR_BIT, 0);
	}
}

bool FramebufferManagerGLES::CreateDownloadTempBuffer(VirtualFramebuffer *nvfb) {
	// When updating VRAM, it need to be exact format.
	if (!gstate_c.Supports(GPU_PREFER_CPU_DOWNLOAD)) {
		switch (nvfb->format) {
		case GE_FORMAT_4444:
			nvfb->colorDepth = Draw::FBO_4444;
			break;
		case GE_FORMAT_5551:
			nvfb->colorDepth = Draw::FBO_5551;
			break;
		case GE_FORMAT_565:
			nvfb->colorDepth = Draw::FBO_565;
			break;
		case GE_FORMAT_8888:
		default:
			nvfb->colorDepth = Draw::FBO_8888;
			break;
		}
	}

	nvfb->fbo = draw_->CreateFramebuffer({ nvfb->bufferWidth, nvfb->bufferHeight, 1, 1, false, (Draw::FBColorDepth)nvfb->colorDepth });
	if (!nvfb->fbo) {
		ERROR_LOG(FRAMEBUF, "Error creating GL FBO! %i x %i", nvfb->renderWidth, nvfb->renderHeight);
		return false;
	}
	return true;
}

void FramebufferManagerGLES::UpdateDownloadTempBuffer(VirtualFramebuffer *nvfb) {
	_assert_msg_(G3D, nvfb->fbo, "Expecting a valid nvfb in UpdateDownloadTempBuffer");

	// Discard the previous contents of this buffer where possible.
	if (gl_extensions.GLES3 && glInvalidateFramebuffer != nullptr) {
		draw_->BindFramebufferAsRenderTarget(nvfb->fbo, { Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE });
	} else if (gl_extensions.IsGLES) {
		draw_->BindFramebufferAsRenderTarget(nvfb->fbo, { Draw::RPAction::CLEAR, Draw::RPAction::CLEAR, Draw::RPAction::CLEAR });
		gstate_c.Dirty(DIRTY_BLEND_STATE);
	}
}

void FramebufferManagerGLES::BlitFramebuffer(VirtualFramebuffer *dst, int dstX, int dstY, VirtualFramebuffer *src, int srcX, int srcY, int w, int h, int bpp) {
	if (!dst->fbo || !src->fbo || !useBufferedRendering_) {
		// This can happen if they recently switched from non-buffered.
		if (useBufferedRendering_)
			draw_->BindFramebufferAsRenderTarget(nullptr, { Draw::RPAction::KEEP, Draw::RPAction::KEEP, Draw::RPAction::KEEP });
		return;
	}

	bool useBlit = gstate_c.Supports(GPU_SUPPORTS_ARB_FRAMEBUFFER_BLIT | GPU_SUPPORTS_NV_FRAMEBUFFER_BLIT);
	bool useNV = useBlit && !gstate_c.Supports(GPU_SUPPORTS_ARB_FRAMEBUFFER_BLIT);

	float srcXFactor = useBlit ? (float)src->renderWidth / (float)src->bufferWidth : 1.0f;
	float srcYFactor = useBlit ? (float)src->renderHeight / (float)src->bufferHeight : 1.0f;
	const int srcBpp = src->format == GE_FORMAT_8888 ? 4 : 2;
	if (srcBpp != bpp && bpp != 0) {
		srcXFactor = (srcXFactor * bpp) / srcBpp;
	}
	int srcX1 = srcX * srcXFactor;
	int srcX2 = (srcX + w) * srcXFactor;
	int srcY1 = srcY * srcYFactor;
	int srcY2 = (srcY + h) * srcYFactor;

	float dstXFactor = useBlit ? (float)dst->renderWidth / (float)dst->bufferWidth : 1.0f;
	float dstYFactor = useBlit ? (float)dst->renderHeight / (float)dst->bufferHeight : 1.0f;
	const int dstBpp = dst->format == GE_FORMAT_8888 ? 4 : 2;
	if (dstBpp != bpp && bpp != 0) {
		dstXFactor = (dstXFactor * bpp) / dstBpp;
	}
	int dstX1 = dstX * dstXFactor;
	int dstX2 = (dstX + w) * dstXFactor;
	int dstY1 = dstY * dstYFactor;
	int dstY2 = (dstY + h) * dstYFactor;

	if (src == dst && srcX == dstX && srcY == dstY) {
		// Let's just skip a copy where the destination is equal to the source.
		WARN_LOG_REPORT_ONCE(blitSame, G3D, "Skipped blit with equal dst and src");
		return;
	}

	if (gstate_c.Supports(GPU_SUPPORTS_ANY_COPY_IMAGE)) {
		// glBlitFramebuffer can clip, but glCopyImageSubData is more restricted.
		// In case the src goes outside, we just skip the optimization in that case.
		const bool sameSize = dstX2 - dstX1 == srcX2 - srcX1 && dstY2 - dstY1 == srcY2 - srcY1;
		const bool sameDepth = dst->colorDepth == src->colorDepth;
		const bool srcInsideBounds = srcX2 <= src->renderWidth && srcY2 <= src->renderHeight;
		const bool dstInsideBounds = dstX2 <= dst->renderWidth && dstY2 <= dst->renderHeight;
		const bool xOverlap = src == dst && srcX2 > dstX1 && srcX1 < dstX2;
		const bool yOverlap = src == dst && srcY2 > dstY1 && srcY1 < dstY2;
		if (sameSize && sameDepth && srcInsideBounds && dstInsideBounds && !(xOverlap && yOverlap)) {
			draw_->CopyFramebufferImage(src->fbo, 0, srcX1, srcY1, 0, dst->fbo, 0, dstX1, dstY1, 0, dstX2 - dstX1, dstY2 - dstY1, 1, Draw::FB_COLOR_BIT);
			return;
		}
	}

	if (useBlit) {
		draw_->BlitFramebuffer(src->fbo, srcX1, srcY1, srcX2, srcY2, dst->fbo, dstX1, dstY1, dstX2, dstY2, Draw::FB_COLOR_BIT, Draw::FB_BLIT_NEAREST);
	} else {
		draw_->BindFramebufferAsRenderTarget(dst->fbo, { Draw::RPAction::KEEP, Draw::RPAction::KEEP, Draw::RPAction::KEEP });
		draw_->BindFramebufferAsTexture(src->fbo, 0, Draw::FB_COLOR_BIT, 0);

		// Make sure our 2D drawing program is ready. Compiles only if not already compiled.
		CompileDraw2DProgram();

		render_->SetViewport({ 0, 0, (float)dst->renderWidth, (float)dst->renderHeight, 0, 1.0f });
		render_->SetStencilDisabled();
		render_->SetDepth(false, false, GL_ALWAYS);
		render_->SetNoBlendAndMask(0xF);

		// The first four coordinates are relative to the 6th and 7th arguments of DrawActiveTexture.
		// Should maybe revamp that interface.
		float srcW = src->bufferWidth;
		float srcH = src->bufferHeight;
		render_->BindProgram(draw2dprogram_);
		DrawActiveTexture(dstX1, dstY1, w * dstXFactor, h, dst->bufferWidth, dst->bufferHeight, srcX1 / srcW, srcY1 / srcH, srcX2 / srcW, srcY2 / srcH, ROTATION_LOCKED_HORIZONTAL, DRAWTEX_NEAREST);
		textureCacheGL_->ForgetLastTexture();
	}

	gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_BLEND_STATE | DIRTY_RASTER_STATE);
}

void FramebufferManagerGLES::EndFrame() {
}

void FramebufferManagerGLES::DeviceLost() {
	DestroyAllFBOs();
	DestroyDeviceObjects();
}

void FramebufferManagerGLES::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	render_ = (GLRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
	CreateDeviceObjects();
}

void FramebufferManagerGLES::DestroyAllFBOs() {
	currentRenderVfb_ = 0;
	displayFramebuf_ = 0;
	prevDisplayFramebuf_ = 0;
	prevPrevDisplayFramebuf_ = 0;

	for (size_t i = 0; i < vfbs_.size(); ++i) {
		VirtualFramebuffer *vfb = vfbs_[i];
		INFO_LOG(FRAMEBUF, "Destroying FBO for %08x : %i x %i x %i", vfb->fb_address, vfb->width, vfb->height, vfb->format);
		DestroyFramebuf(vfb);
	}
	vfbs_.clear();

	for (size_t i = 0; i < bvfbs_.size(); ++i) {
		VirtualFramebuffer *vfb = bvfbs_[i];
		DestroyFramebuf(vfb);
	}
	bvfbs_.clear();

	for (auto &tempFB : tempFBOs_) {
		tempFB.second.fbo->Release();
	}
	tempFBOs_.clear();

	SetNumExtraFBOs(0);
}

void FramebufferManagerGLES::Resized() {
	FramebufferManagerCommon::Resized();

	render_->Resize(PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight);
	if (UpdateSize()) {
		DestroyAllFBOs();
	}

	// Might have a new post shader - let's compile it.
	CompilePostShader();

	// render_->SetLineWidth(renderWidth_ / 480.0f);
}

bool FramebufferManagerGLES::GetOutputFramebuffer(GPUDebugBuffer &buffer) {
	int w, h;
	draw_->GetFramebufferDimensions(nullptr, &w, &h);
	buffer.Allocate(w, h, GPU_DBG_FORMAT_888_RGB, true);
	draw_->CopyFramebufferToMemorySync(nullptr, Draw::FB_COLOR_BIT, 0, 0, w, h, Draw::DataFormat::R8G8B8_UNORM, buffer.GetData(), w);
	return true;
}
