// Copyright (c) 2013- PPSSPP Project.

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

#include <cmath>
#include <algorithm>

#include "Common/Common.h"
#include "Common/CPUDetect.h"
#include "Common/Math/math_util.h"
#include "Common/MemoryUtil.h"
#include "Common/Profiler/Profiler.h"
#include "Core/Config.h"
#include "GPU/GPUState.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/TextureDecoder.h"
#include "GPU/Debugger/Debugger.h"
#include "GPU/Software/BinManager.h"
#include "GPU/Software/Clipper.h"
#include "GPU/Software/FuncId.h"
#include "GPU/Software/Lighting.h"
#include "GPU/Software/Rasterizer.h"
#include "GPU/Software/RasterizerRectangle.h"
#include "GPU/Software/TransformUnit.h"

#define TRANSFORM_BUF_SIZE (65536 * 48)

TransformUnit::TransformUnit() {
	decoded_ = (u8 *)AllocateMemoryPages(TRANSFORM_BUF_SIZE, MEM_PROT_READ | MEM_PROT_WRITE);
	binner_ = new BinManager();
}

TransformUnit::~TransformUnit() {
	FreeMemoryPages(decoded_, TRANSFORM_BUF_SIZE);
	delete binner_;
}

SoftwareDrawEngine::SoftwareDrawEngine() {
	// All this is a LOT of memory, need to see if we can cut down somehow.  Used for splines.
	decoded = (u8 *)AllocateMemoryPages(DECODED_VERTEX_BUFFER_SIZE, MEM_PROT_READ | MEM_PROT_WRITE);
	decIndex = (u16 *)AllocateMemoryPages(DECODED_INDEX_BUFFER_SIZE, MEM_PROT_READ | MEM_PROT_WRITE);
}

SoftwareDrawEngine::~SoftwareDrawEngine() {
	FreeMemoryPages(decoded, DECODED_VERTEX_BUFFER_SIZE);
	FreeMemoryPages(decIndex, DECODED_INDEX_BUFFER_SIZE);
}

void SoftwareDrawEngine::DispatchFlush() {
	transformUnit.Flush("debug");
}

void SoftwareDrawEngine::DispatchSubmitPrim(const void *verts, const void *inds, GEPrimitiveType prim, int vertexCount, u32 vertTypeID, int cullMode, int *bytesRead) {
	_assert_msg_(cullMode == gstate.getCullMode(), "Mixed cull mode not supported.");
	transformUnit.SubmitPrimitive(verts, inds, prim, vertexCount, vertTypeID, bytesRead, this);
}

void SoftwareDrawEngine::DispatchSubmitImm(GEPrimitiveType prim, TransformedVertex *buffer, int vertexCount, int cullMode, bool continuation) {
	uint32_t vertTypeID = GetVertTypeID(gstate.vertType | GE_VTYPE_POS_FLOAT, gstate.getUVGenMode());

	int flipCull = cullMode != gstate.getCullMode() ? 1 : 0;
	// TODO: For now, just setting all dirty.
	transformUnit.SetDirty(SoftDirty(-1));
	gstate.cullmode ^= flipCull;

	// TODO: This is a bit ugly.  Should bypass when clipping...
	uint32_t xScale = gstate.viewportxscale;
	uint32_t xCenter = gstate.viewportxcenter;
	uint32_t yScale = gstate.viewportyscale;
	uint32_t yCenter = gstate.viewportycenter;
	uint32_t zScale = gstate.viewportzscale;
	uint32_t zCenter = gstate.viewportzcenter;

	// Force scale to 1 and center to zero.
	gstate.viewportxscale = (GE_CMD_VIEWPORTXSCALE << 24) | 0x3F8000;
	gstate.viewportxcenter = (GE_CMD_VIEWPORTXCENTER << 24) | 0x000000;
	gstate.viewportyscale = (GE_CMD_VIEWPORTYSCALE << 24) | 0x3F8000;
	gstate.viewportycenter = (GE_CMD_VIEWPORTYCENTER << 24) | 0x000000;
	// Z we scale to 65535 for neg z clipping.
	gstate.viewportzscale = (GE_CMD_VIEWPORTZSCALE << 24) | 0x477FFF;
	gstate.viewportzcenter = (GE_CMD_VIEWPORTZCENTER << 24) | 0x000000;

	// Before we start, submit 0 prims to reset the prev prim type.
	// Following submits will always be KEEP_PREVIOUS.
	if (!continuation)
		transformUnit.SubmitPrimitive(nullptr, nullptr, prim, 0, vertTypeID, nullptr, this);

	for (int i = 0; i < vertexCount; i++) {
		VertexData vert;
		vert.clippos = ClipCoords(buffer[i].pos);
		vert.texturecoords.x = buffer[i].u;
		vert.texturecoords.y = buffer[i].v;
		if (gstate.isModeThrough()) {
			vert.texturecoords.x *= gstate.getTextureWidth(0);
			vert.texturecoords.y *= gstate.getTextureHeight(0);
		} else {
			vert.clippos.z *= 1.0f / 65535.0f;
		}
		vert.color0 = buffer[i].color0_32;
		vert.color1 = gstate.isUsingSecondaryColor() && !gstate.isModeThrough() ? buffer[i].color1_32 : 0;
		vert.fogdepth = buffer[i].fog;
		vert.screenpos.x = (int)(buffer[i].x * 16.0f);
		vert.screenpos.y = (int)(buffer[i].y * 16.0f);
		vert.screenpos.z = (u16)(u32)buffer[i].z;

		transformUnit.SubmitImmVertex(vert, this);
	}

	gstate.viewportxscale = xScale;
	gstate.viewportxcenter = xCenter;
	gstate.viewportyscale = yScale;
	gstate.viewportycenter = yCenter;
	gstate.viewportzscale = zScale;
	gstate.viewportzcenter = zCenter;

	gstate.cullmode ^= flipCull;
	// TODO: Should really clear, but a bunch of values are forced so we this is safest.
	transformUnit.SetDirty(SoftDirty(-1));
}

VertexDecoder *SoftwareDrawEngine::FindVertexDecoder(u32 vtype) {
	const u32 vertTypeID = (vtype & 0xFFFFFF) | (gstate.getUVGenMode() << 24);
	return DrawEngineCommon::GetVertexDecoder(vertTypeID);
}

WorldCoords TransformUnit::ModelToWorld(const ModelCoords &coords) {
	return Vec3ByMatrix43(coords, gstate.worldMatrix);
}

WorldCoords TransformUnit::ModelToWorldNormal(const ModelCoords &coords) {
	return Norm3ByMatrix43(coords, gstate.worldMatrix);
}

ViewCoords TransformUnit::WorldToView(const WorldCoords &coords) {
	return Vec3ByMatrix43(coords, gstate.viewMatrix);
}

ClipCoords TransformUnit::ViewToClip(const ViewCoords &coords) {
	return Vec3ByMatrix44(coords, gstate.projMatrix);
}

template <bool depthClamp, bool writeOutsideFlag>
static ScreenCoords ClipToScreenInternal(Vec3f scaled, const ClipCoords &coords, bool *outside_range_flag) {
	ScreenCoords ret;

	// Account for rounding for X and Y.
	// TODO: Validate actual rounding range.
	const float SCREEN_BOUND = 4095.0f + (15.5f / 16.0f);

	// This matches hardware tests - depth is clamped when this flag is on.
	if (depthClamp) {
		// Note: if the depth is clipped (z/w <= -1.0), the outside_range_flag should NOT be set, even for x and y.
		if (writeOutsideFlag && coords.z > -coords.w && (scaled.x >= SCREEN_BOUND || scaled.y >= SCREEN_BOUND || scaled.x < 0 || scaled.y < 0)) {
			*outside_range_flag = true;
		}

		if (scaled.z < 0.f)
			scaled.z = 0.f;
		else if (scaled.z > 65535.0f)
			scaled.z = 65535.0f;
	} else if (writeOutsideFlag && (scaled.x > SCREEN_BOUND || scaled.y >= SCREEN_BOUND || scaled.x < 0 || scaled.y < 0)) {
		*outside_range_flag = true;
	}

	// 16 = 0xFFFF / 4095.9375
	// Round up at 0.625 to the nearest subpixel.
	static_assert(SCREEN_SCALE_FACTOR == 16, "Currently only supports scale 16");
	int x = (int)(scaled.x * 16.0f + 0.375f - gstate.getOffsetX16());
	int y = (int)(scaled.y * 16.0f + 0.375f - gstate.getOffsetY16());
	return ScreenCoords(x, y, scaled.z);
}

static inline ScreenCoords ClipToScreenInternal(const ClipCoords &coords, bool *outside_range_flag) {
	// Parameters here can seem invalid, but the PSP is fine with negative viewport widths etc.
	// The checking that OpenGL and D3D do is actually quite superflous as the calculations still "work"
	// with some pretty crazy inputs, which PSP games are happy to do at times.
	float xScale = gstate.getViewportXScale();
	float xCenter = gstate.getViewportXCenter();
	float yScale = gstate.getViewportYScale();
	float yCenter = gstate.getViewportYCenter();
	float zScale = gstate.getViewportZScale();
	float zCenter = gstate.getViewportZCenter();

	float x = coords.x * xScale / coords.w + xCenter;
	float y = coords.y * yScale / coords.w + yCenter;
	float z = coords.z * zScale / coords.w + zCenter;

	if (gstate.isDepthClampEnabled()) {
		if (outside_range_flag)
			return ClipToScreenInternal<true, true>(Vec3f(x, y, z), coords, outside_range_flag);
		return ClipToScreenInternal<true, false>(Vec3f(x, y, z), coords, outside_range_flag);
	}
	if (outside_range_flag)
		return ClipToScreenInternal<false, true>(Vec3f(x, y, z), coords, outside_range_flag);
	return ClipToScreenInternal<false, false>(Vec3f(x, y, z), coords, outside_range_flag);
}

ScreenCoords TransformUnit::ClipToScreen(const ClipCoords &coords) {
	return ClipToScreenInternal(coords, nullptr);
}

ScreenCoords TransformUnit::DrawingToScreen(const DrawingCoords &coords, u16 z) {
	ScreenCoords ret;
	ret.x = (u32)coords.x * SCREEN_SCALE_FACTOR;
	ret.y = (u32)coords.y * SCREEN_SCALE_FACTOR;
	ret.z = z;
	return ret;
}

enum class MatrixMode {
	NONE = 0,
	POS_TO_CLIP = 1,
	POS_TO_VIEW = 2,
	WORLD_TO_CLIP = 3,
};

struct TransformState {
	Lighting::State lightingState;

	float fogEnd;
	float fogSlope;

	float matrix[16];
	Vec3f screenScale;
	Vec3f screenAdd;

	ScreenCoords(*roundToScreen)(Vec3f scaled, const ClipCoords &coords, bool *outside_range_flag);

	struct {
		bool enableTransform : 1;
		bool enableLighting : 1;
		bool enableFog : 1;
		bool readUV : 1;
		bool readWeights : 1;
		bool negateNormals : 1;
		uint8_t uvGenMode : 2;
		uint8_t matrixMode : 2;
	};
};

void ComputeTransformState(TransformState *state, const VertexReader &vreader) {
	state->enableTransform = !vreader.isThrough();
	state->enableLighting = gstate.isLightingEnabled();
	state->enableFog = gstate.isFogEnabled();
	state->readUV = !gstate.isModeClear() && gstate.isTextureMapEnabled() && vreader.hasUV();
	state->readWeights = vreader.skinningEnabled() && state->enableTransform;
	state->negateNormals = gstate.areNormalsReversed();

	state->uvGenMode = gstate.getUVGenMode();

	if (state->enableTransform) {
		if (state->enableFog) {
			state->fogEnd = getFloat24(gstate.fog1);
			state->fogSlope = getFloat24(gstate.fog2);
			// Same fixup as in ShaderManagerGLES.cpp
			if (my_isnanorinf(state->fogEnd)) {
				state->fogEnd = std::signbit(state->fogEnd) ? -INFINITY : INFINITY;
			}
			if (my_isnanorinf(state->fogSlope)) {
				state->fogSlope = std::signbit(state->fogSlope) ? -INFINITY : INFINITY;
			}
		}

		bool canSkipWorldPos = true;
		bool canSkipViewPos = !state->enableFog;
		if (state->enableLighting) {
			Lighting::ComputeState(&state->lightingState, vreader.hasColor0());
			for (int i = 0; i < 4; ++i) {
				if (!state->lightingState.lights[i].enabled)
					continue;
				if (!state->lightingState.lights[i].directional)
					canSkipWorldPos = false;
			}
		}

		float world[16];
		float view[16];
		if (canSkipWorldPos && canSkipViewPos) {
			state->matrixMode = (uint8_t)MatrixMode::POS_TO_CLIP;

			ConvertMatrix4x3To4x4(world, gstate.worldMatrix);
			ConvertMatrix4x3To4x4(view, gstate.viewMatrix);

			float worldview[16];
			Matrix4ByMatrix4(worldview, world, view);
			Matrix4ByMatrix4(state->matrix, worldview, gstate.projMatrix);
		} else if (canSkipWorldPos) {
			state->matrixMode = (uint8_t)MatrixMode::POS_TO_VIEW;

			ConvertMatrix4x3To4x4(world, gstate.worldMatrix);
			ConvertMatrix4x3To4x4(view, gstate.viewMatrix);

			Matrix4ByMatrix4(state->matrix, world, view);
		} else if (canSkipViewPos) {
			state->matrixMode = (uint8_t)MatrixMode::WORLD_TO_CLIP;

			ConvertMatrix4x3To4x4(view, gstate.viewMatrix);
			Matrix4ByMatrix4(state->matrix, view, gstate.projMatrix);
		} else {
			state->matrixMode = (uint8_t)MatrixMode::NONE;
		}

		state->screenScale = Vec3f(gstate.getViewportXScale(), gstate.getViewportYScale(), gstate.getViewportZScale());
		state->screenAdd = Vec3f(gstate.getViewportXCenter(), gstate.getViewportYCenter(), gstate.getViewportZCenter());
	}

	if (gstate.isDepthClampEnabled())
		state->roundToScreen = &ClipToScreenInternal<true, true>;
	else
		state->roundToScreen = &ClipToScreenInternal<false, true>;
}

VertexData TransformUnit::ReadVertex(VertexReader &vreader, const TransformState &state) {
	PROFILE_THIS_SCOPE("read_vert");
	VertexData vertex;

	ModelCoords pos;
	// VertexDecoder normally scales z, but we want it unscaled.
	vreader.ReadPosThroughZ16(pos.AsArray());

	if (state.readUV) {
		vreader.ReadUV(vertex.texturecoords.AsArray());
	} else {
		vertex.texturecoords.SetZero();
	}

	Vec3<float> normal;
	if (vreader.hasNormal()) {
		vreader.ReadNrm(normal.AsArray());

		if (state.negateNormals)
			normal = -normal;
	}

	if (state.readWeights) {
		float W[8] = { 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
		vreader.ReadWeights(W);

		Vec3<float> tmppos(0.f, 0.f, 0.f);
		Vec3<float> tmpnrm(0.f, 0.f, 0.f);

		for (int i = 0; i < vreader.numBoneWeights(); ++i) {
			Vec3<float> step = Vec3ByMatrix43(pos, gstate.boneMatrix + i * 12);
			tmppos += step * W[i];
			if (vreader.hasNormal()) {
				step = Norm3ByMatrix43(normal, gstate.boneMatrix + i * 12);
				tmpnrm += step * W[i];
			}
		}

		pos = tmppos;
		if (vreader.hasNormal())
			normal = tmpnrm;
	}

	if (vreader.hasColor0()) {
		vreader.ReadColor0_8888((u8 *)&vertex.color0);
	} else {
		vertex.color0 = gstate.getMaterialAmbientRGBA();
	}

	vertex.color1 = 0;

	if (state.enableTransform) {
		WorldCoords worldpos;
		ModelCoords viewpos;

		switch (MatrixMode(state.matrixMode)) {
		case MatrixMode::NONE:
			worldpos = TransformUnit::ModelToWorld(pos);
			viewpos = TransformUnit::WorldToView(worldpos);
			vertex.clippos = TransformUnit::ViewToClip(viewpos);
			break;

		case MatrixMode::POS_TO_CLIP:
			vertex.clippos = Vec3ByMatrix44(pos, state.matrix);
			break;

		case MatrixMode::POS_TO_VIEW:
#ifdef _M_SSE
			viewpos = Vec3ByMatrix44(pos, state.matrix).vec;
#else
			viewpos = Vec3ByMatrix44(pos, state.matrix).rgb();
#endif
			vertex.clippos = TransformUnit::ViewToClip(viewpos);
			break;

		case MatrixMode::WORLD_TO_CLIP:
			worldpos = TransformUnit::ModelToWorld(pos);
			vertex.clippos = Vec3ByMatrix44(worldpos, state.matrix);
			break;
		}

		Vec3f screenScaled;
#ifdef _M_SSE
		screenScaled.vec = _mm_mul_ps(vertex.clippos.vec, state.screenScale.vec);
		screenScaled.vec = _mm_div_ps(screenScaled.vec, _mm_shuffle_ps(vertex.clippos.vec, vertex.clippos.vec, _MM_SHUFFLE(3, 3, 3, 3)));
		screenScaled.vec = _mm_add_ps(screenScaled.vec, state.screenAdd.vec);
#else
		screenScaled = vertex.clippos.xyz() * state.screenScale / vertex.clippos.w + state.screenAdd;
#endif
		bool outside_range_flag = false;
		vertex.screenpos = state.roundToScreen(screenScaled, vertex.clippos, &outside_range_flag);
		if (outside_range_flag) {
			// We use this, essentially, as the flag.
			vertex.screenpos.x = 0x7FFFFFFF;
			return vertex;
		}

		if (state.enableFog) {
			vertex.fogdepth = (viewpos.z + state.fogEnd) * state.fogSlope;
		} else {
			vertex.fogdepth = 1.0f;
		}

		Vec3<float> worldnormal;
		if (vreader.hasNormal()) {
			worldnormal = TransformUnit::ModelToWorldNormal(normal);
			worldnormal.NormalizeOr001();
		} else {
			worldnormal = Vec3<float>(0.0f, 0.0f, 1.0f);
		}

		// Time to generate some texture coords.  Lighting will handle shade mapping.
		if (state.uvGenMode == GE_TEXMAP_TEXTURE_MATRIX) {
			Vec3f source;
			switch (gstate.getUVProjMode()) {
			case GE_PROJMAP_POSITION:
				source = pos;
				break;

			case GE_PROJMAP_UV:
				source = Vec3f(vertex.texturecoords, 0.0f);
				break;

			case GE_PROJMAP_NORMALIZED_NORMAL:
				source = normal.NormalizedOr001(cpu_info.bSSE4_1);
				break;

			case GE_PROJMAP_NORMAL:
				source = normal;
				break;

			default:
				source = Vec3f::AssignToAll(0.0f);
				ERROR_LOG_REPORT(G3D, "Software: Unsupported UV projection mode %x", gstate.getUVProjMode());
				break;
			}

			// TODO: What about uv scale and offset?
			Vec3<float> stq = Vec3ByMatrix43(source, gstate.tgenMatrix);
			float z_recip = 1.0f / stq.z;
			vertex.texturecoords = Vec2f(stq.x * z_recip, stq.y * z_recip);
		} else if (state.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP) {
			Lighting::GenerateLightST(vertex, worldnormal);
		}

		PROFILE_THIS_SCOPE("light");
		if (state.enableLighting)
			Lighting::Process(vertex, worldpos, worldnormal, state.lightingState);
	} else {
		vertex.screenpos.x = (int)(pos[0] * SCREEN_SCALE_FACTOR);
		vertex.screenpos.y = (int)(pos[1] * SCREEN_SCALE_FACTOR);
		vertex.screenpos.z = pos[2];
		vertex.clippos.w = 1.f;
		vertex.fogdepth = 1.f;
	}

	return vertex;
}

void TransformUnit::SetDirty(SoftDirty flags) {
	binner_->SetDirty(flags);
}
SoftDirty TransformUnit::GetDirty() {
	return binner_->GetDirty();
}

void TransformUnit::SubmitPrimitive(const void* vertices, const void* indices, GEPrimitiveType prim_type, int vertex_count, u32 vertex_type, int *bytesRead, SoftwareDrawEngine *drawEngine)
{
	VertexDecoder &vdecoder = *drawEngine->FindVertexDecoder(vertex_type);
	const DecVtxFormat &vtxfmt = vdecoder.GetDecVtxFmt();

	if (bytesRead)
		*bytesRead = vertex_count * vdecoder.VertexSize();

	// Frame skipping.
	if (gstate_c.skipDrawReason & SKIPDRAW_SKIPFRAME) {
		return;
	}
	// Vertices without position are just entirely culled.
	// Note: Throughmode does draw 8-bit primitives, but positions are always zero - handled in decode.
	if ((vertex_type & GE_VTYPE_POS_MASK) == 0)
		return;

	u16 index_lower_bound = 0;
	u16 index_upper_bound = vertex_count == 0 ? 0 : vertex_count - 1;
	IndexConverter ConvertIndex(vertex_type, indices);

	if (indices)
		GetIndexBounds(indices, vertex_count, vertex_type, &index_lower_bound, &index_upper_bound);
	if (vertex_count != 0)
		vdecoder.DecodeVerts(decoded_, vertices, index_lower_bound, index_upper_bound);

	VertexReader vreader(decoded_, vtxfmt, vertex_type);

	if (prim_type != GE_PRIM_KEEP_PREVIOUS) {
		data_index_ = 0;
		prev_prim_ = prim_type;
	} else {
		prim_type = prev_prim_;
	}

	// TODO: Do this in two passes - first process the vertices (before indexing/stripping),
	// then resolve the indices. This lets us avoid transforming shared vertices twice.

	binner_->UpdateState(vreader.isThrough());
	hasDraws_ = true;

	static TransformState transformState;
	if (binner_->HasDirty(SoftDirty::LIGHT_ALL | SoftDirty::TRANSFORM_ALL)) {
		ComputeTransformState(&transformState, vreader);
		binner_->ClearDirty(SoftDirty::LIGHT_ALL | SoftDirty::TRANSFORM_ALL);
	}

	bool skipCull = !gstate.isCullEnabled() || gstate.isModeClear();
	const CullType cullType = skipCull ? CullType::OFF : (gstate.getCullMode() ? CullType::CCW : CullType::CW);

	auto readVertexAt = [&](VertexReader &vreader, const TransformState &transformState, int vtx) {
		if (indices) {
			vreader.Goto(ConvertIndex(vtx) - index_lower_bound);
		} else {
			vreader.Goto(vtx);
		}

		return ReadVertex(vreader, transformState);
	};

	if (vreader.isThrough() && cullType == CullType::OFF && prim_type == GE_PRIM_TRIANGLES && data_index_ == 0 && vertex_count >= 6 && ((vertex_count) % 6) == 0) {
		// Some games send rectangles as a series of regular triangles.
		// We look for this, but only in throughmode.
		VertexData buf[6];
		int buf_index = data_index_;
		for (int i = 0; i < data_index_; ++i) {
			buf[i] = data_[i];
		}

		for (int vtx = 0; vtx < vertex_count; ++vtx) {
			buf[buf_index++] = readVertexAt(vreader, transformState, vtx);
			if (buf_index < 6)
				continue;

			int tl = -1, br = -1;
			if (Rasterizer::DetectRectangleFromPair(binner_->State(), buf, &tl, &br)) {
				Clipper::ProcessRect(buf[tl], buf[br], *binner_);
			} else {
				SendTriangle(cullType, &buf[0]);
				SendTriangle(cullType, &buf[3]);
			}

			buf_index = 0;
		}

		if (buf_index >= 3) {
			SendTriangle(cullType, &buf[0]);
			data_index_ = 0;
			for (int i = 3; i < buf_index; ++i) {
				data_[data_index_++] = buf[i];
			}
		} else if (buf_index > 0) {
			for (int i = 0; i < buf_index; ++i) {
				data_[i] = buf[i];
			}
			data_index_ = buf_index;
		} else {
			data_index_ = 0;
		}

		return;
	}

	// Note: intentionally, these allow for the case of vertex_count == 0, but data_index_ > 0.
	// This is used for immediate-mode primitives.
	switch (prim_type) {
	case GE_PRIM_POINTS:
		for (int i = 0; i < data_index_; ++i)
			Clipper::ProcessPoint(data_[i], *binner_);
		data_index_ = 0;
		for (int vtx = 0; vtx < vertex_count; ++vtx) {
			data_[0] = readVertexAt(vreader, transformState, vtx);
			Clipper::ProcessPoint(data_[0], *binner_);
		}
		break;

	case GE_PRIM_LINES:
		for (int i = 0; i < data_index_ - 1; i += 2)
			Clipper::ProcessLine(data_[i + 0], data_[i + 1], *binner_);
		data_index_ &= 1;
		for (int vtx = 0; vtx < vertex_count; ++vtx) {
			data_[data_index_++] = readVertexAt(vreader, transformState, vtx);
			if (data_index_ == 2) {
				Clipper::ProcessLine(data_[0], data_[1], *binner_);
				data_index_ = 0;
			}
		}
		break;

	case GE_PRIM_TRIANGLES:
		for (int vtx = 0; vtx < vertex_count; ++vtx) {
			data_[data_index_++] = readVertexAt(vreader, transformState, vtx);
			if (data_index_ < 3) {
				// Keep reading.  Note: an incomplete prim will stay read for GE_PRIM_KEEP_PREVIOUS.
				continue;
			}
			// Okay, we've got enough verts.  Reset the index for next time.
			data_index_ = 0;

			SendTriangle(cullType, &data_[0]);
		}
		// In case vertex_count was 0.
		if (data_index_ >= 3) {
			SendTriangle(cullType, &data_[0]);
			data_index_ = 0;
		}
		break;

	case GE_PRIM_RECTANGLES:
		for (int vtx = 0; vtx < vertex_count; ++vtx) {
			data_[data_index_++] = readVertexAt(vreader, transformState, vtx);

			if (data_index_ == 4 && vreader.isThrough() && cullType == CullType::OFF) {
				if (Rasterizer::DetectRectangleThroughModeSlices(binner_->State(), data_)) {
					data_[1] = data_[3];
					data_index_ = 2;
				}
			}

			if (data_index_ == 4) {
				Clipper::ProcessRect(data_[0], data_[1], *binner_);
				Clipper::ProcessRect(data_[2], data_[3], *binner_);
				data_index_ = 0;
			}
		}

		if (data_index_ >= 2) {
			Clipper::ProcessRect(data_[0], data_[1], *binner_);
			data_index_ -= 2;
		}
		break;

	case GE_PRIM_LINE_STRIP:
		{
			// Don't draw a line when loading the first vertex.
			// If data_index_ is 1 or 2, etc., it means we're continuing a line strip.
			int skip_count = data_index_ == 0 ? 1 : 0;
			for (int vtx = 0; vtx < vertex_count; ++vtx) {
				data_[(data_index_++) & 1] = readVertexAt(vreader, transformState, vtx);

				if (skip_count) {
					--skip_count;
				} else {
					// We already incremented data_index_, so data_index_ & 1 is previous one.
					Clipper::ProcessLine(data_[data_index_ & 1], data_[(data_index_ & 1) ^ 1], *binner_);
				}
			}
			// If this is from immediate-mode drawing, we always had one new vert (already in data_.)
			if (isImmDraw_ && data_index_ >= 2)
				Clipper::ProcessLine(data_[data_index_ & 1], data_[(data_index_ & 1) ^ 1], *binner_);
			break;
		}

	case GE_PRIM_TRIANGLE_STRIP:
		{
			// Don't draw a triangle when loading the first two vertices.
			int skip_count = data_index_ >= 2 ? 0 : 2 - data_index_;
			int start_vtx = 0;

			// If index count == 4, check if we can convert to a rectangle.
			// This is for Darkstalkers (and should speed up many 2D games).
			if (data_index_ == 0 && vertex_count >= 4 && (vertex_count & 1) == 0 && cullType == CullType::OFF) {
				for (int base = 0; base < vertex_count - 2; base += 2) {
					for (int vtx = base == 0 ? 0 : 2; vtx < 4; ++vtx) {
						data_[vtx] = readVertexAt(vreader, transformState, base + vtx);
					}

					// If a strip is effectively a rectangle, draw it as such!
					int tl = -1, br = -1;
					if (Rasterizer::DetectRectangleFromStrip(binner_->State(), data_, &tl, &br)) {
						Clipper::ProcessRect(data_[tl], data_[br], *binner_);
						start_vtx += 2;
						skip_count = 0;
						if (base + 4 >= vertex_count) {
							start_vtx = vertex_count;
							break;
						}

						// Just copy the first two so we can detect easier.
						// TODO: Maybe should give detection two halves?
						data_[0] = data_[2];
						data_[1] = data_[3];
					} else {
						// Go into triangle mode.  Unfortunately, we re-read the verts.
						break;
					}
				}
			}

			for (int vtx = start_vtx; vtx < vertex_count && skip_count > 0; ++vtx) {
				int provoking_index = (data_index_++) % 3;
				data_[provoking_index] = readVertexAt(vreader, transformState, vtx);
				--skip_count;
				++start_vtx;
			}

			for (int vtx = start_vtx; vtx < vertex_count; ++vtx) {
				int provoking_index = (data_index_++) % 3;
				data_[provoking_index] = readVertexAt(vreader, transformState, vtx);

				int wind = (data_index_ - 1) % 2;
				CullType altCullType = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
				SendTriangle(altCullType, &data_[0], provoking_index);
			}

			// If this is from immediate-mode drawing, we always had one new vert (already in data_.)
			if (isImmDraw_ && data_index_ >= 3) {
				int provoking_index = (data_index_ - 1) % 3;
				int wind = (data_index_ - 1) % 2;
				CullType altCullType = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
				SendTriangle(altCullType, &data_[0], provoking_index);
			}
			break;
		}

	case GE_PRIM_TRIANGLE_FAN:
		{
			// Don't draw a triangle when loading the first two vertices.
			// (this doesn't count the central one.)
			int skip_count = data_index_ <= 1 ? 1 : 0;
			int start_vtx = 0;

			// Only read the central vertex if we're not continuing.
			if (data_index_ == 0 && vertex_count > 0) {
				data_[0] = readVertexAt(vreader, transformState, 0);
				data_index_++;
				start_vtx = 1;
			}

			if (data_index_ == 1 && vertex_count == 4 && cullType == CullType::OFF) {
				for (int vtx = start_vtx; vtx < vertex_count; ++vtx) {
					data_[vtx] = readVertexAt(vreader, transformState, vtx);
				}

				int tl = -1, br = -1;
				if (Rasterizer::DetectRectangleFromFan(binner_->State(), data_, vertex_count, &tl, &br)) {
					Clipper::ProcessRect(data_[tl], data_[br], *binner_);
					break;
				}
			}

			for (int vtx = start_vtx; vtx < vertex_count && skip_count > 0; ++vtx) {
				int provoking_index = 2 - ((data_index_++) % 2);
				data_[provoking_index] = readVertexAt(vreader, transformState, vtx);
				--skip_count;
				++start_vtx;
			}

			for (int vtx = start_vtx; vtx < vertex_count; ++vtx) {
				int provoking_index = 2 - ((data_index_++) % 2);
				data_[provoking_index] = readVertexAt(vreader, transformState, vtx);

				int wind = (data_index_ - 1) % 2;
				CullType altCullType = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
				SendTriangle(altCullType, &data_[0], provoking_index);
			}

			// If this is from immediate-mode drawing, we always had one new vert (already in data_.)
			if (isImmDraw_ && data_index_ >= 3) {
				int wind = (data_index_ - 1) % 2;
				int provoking_index = 2 - wind;
				CullType altCullType = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
				SendTriangle(altCullType, &data_[0], provoking_index);
			}
			break;
		}

	default:
		ERROR_LOG(G3D, "Unexpected prim type: %d", prim_type);
		break;
	}
}

void TransformUnit::SubmitImmVertex(const VertexData &vert, SoftwareDrawEngine *drawEngine) {
	// Where we put it is different for STRIP/FAN types.
	switch (prev_prim_) {
	case GE_PRIM_POINTS:
	case GE_PRIM_LINES:
	case GE_PRIM_TRIANGLES:
	case GE_PRIM_RECTANGLES:
		// This is the easy one.  SubmitPrimitive resets data_index_.
		data_[data_index_++] = vert;
		break;

	case GE_PRIM_LINE_STRIP:
		// This one alternates, and data_index_ > 0 means it draws a segment.
		data_[(data_index_++) & 1] = vert;
		break;

	case GE_PRIM_TRIANGLE_STRIP:
		data_[(data_index_++) % 3] = vert;
		break;

	case GE_PRIM_TRIANGLE_FAN:
		if (data_index_ == 0) {
			data_[data_index_++] = vert;
		} else {
			int provoking_index = 2 - ((data_index_++) % 2);
			data_[provoking_index] = vert;
		}
		break;

	default:
		_assert_msg_(false, "Invalid prim type: %d", (int)prev_prim_);
		break;
	}

	uint32_t vertTypeID = GetVertTypeID(gstate.vertType | GE_VTYPE_POS_FLOAT, gstate.getUVGenMode());
	// This now processes the step with shared logic, given the existing data_.
	isImmDraw_ = true;
	SubmitPrimitive(nullptr, nullptr, GE_PRIM_KEEP_PREVIOUS, 0, vertTypeID, nullptr, drawEngine);
	isImmDraw_ = false;
}

void TransformUnit::SendTriangle(CullType cullType, const VertexData *verts, int provoking) {
	if (cullType == CullType::OFF) {
		Clipper::ProcessTriangle(verts[0], verts[1], verts[2], verts[provoking], *binner_);
		Clipper::ProcessTriangle(verts[2], verts[1], verts[0], verts[provoking], *binner_);
	} else if (cullType == CullType::CW) {
		Clipper::ProcessTriangle(verts[2], verts[1], verts[0], verts[provoking], *binner_);
	} else {
		Clipper::ProcessTriangle(verts[0], verts[1], verts[2], verts[provoking], *binner_);
	}
}

void TransformUnit::Flush(const char *reason) {
	if (!hasDraws_)
		return;

	binner_->Flush(reason);
	GPUDebug::NotifyDraw();
	hasDraws_ = false;
}

void TransformUnit::GetStats(char *buffer, size_t bufsize) {
	// TODO: More stats?
	binner_->GetStats(buffer, bufsize);
}

void TransformUnit::FlushIfOverlap(const char *reason, bool modifying, uint32_t addr, uint32_t stride, uint32_t w, uint32_t h) {
	if (!hasDraws_)
		return;

	if (binner_->HasPendingWrite(addr, stride, w, h))
		Flush(reason);
	if (modifying && binner_->HasPendingRead(addr, stride, w, h))
		Flush(reason);
}

void TransformUnit::NotifyClutUpdate(const void *src) {
	binner_->UpdateClut(src);
}

// TODO: This probably is not the best interface.
// Also, we should try to merge this into the similar function in DrawEngineCommon.
bool TransformUnit::GetCurrentSimpleVertices(int count, std::vector<GPUDebugVertex> &vertices, std::vector<u16> &indices) {
	// This is always for the current vertices.
	u16 indexLowerBound = 0;
	u16 indexUpperBound = count - 1;

	if (!Memory::IsValidAddress(gstate_c.vertexAddr) || count == 0)
		return false;

	if (count > 0 && (gstate.vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
		const u8 *inds = Memory::GetPointer(gstate_c.indexAddr);
		const u16_le *inds16 = (const u16_le *)inds;
		const u32_le *inds32 = (const u32_le *)inds;

		if (inds) {
			GetIndexBounds(inds, count, gstate.vertType, &indexLowerBound, &indexUpperBound);
			indices.resize(count);
			switch (gstate.vertType & GE_VTYPE_IDX_MASK) {
			case GE_VTYPE_IDX_8BIT:
				for (int i = 0; i < count; ++i) {
					indices[i] = inds[i];
				}
				break;
			case GE_VTYPE_IDX_16BIT:
				for (int i = 0; i < count; ++i) {
					indices[i] = inds16[i];
				}
				break;
			case GE_VTYPE_IDX_32BIT:
				WARN_LOG_REPORT_ONCE(simpleIndexes32, G3D, "SimpleVertices: Decoding 32-bit indexes");
				for (int i = 0; i < count; ++i) {
					// These aren't documented and should be rare.  Let's bounds check each one.
					if (inds32[i] != (u16)inds32[i]) {
						ERROR_LOG_REPORT_ONCE(simpleIndexes32Bounds, G3D, "SimpleVertices: Index outside 16-bit range");
					}
					indices[i] = (u16)inds32[i];
				}
				break;
			}
		} else {
			indices.clear();
		}
	} else {
		indices.clear();
	}

	static std::vector<u32> temp_buffer;
	static std::vector<SimpleVertex> simpleVertices;
	temp_buffer.resize(std::max((int)indexUpperBound, 8192) * 128 / sizeof(u32));
	simpleVertices.resize(indexUpperBound + 1);

	VertexDecoder vdecoder;
	VertexDecoderOptions options{};
	vdecoder.SetVertexType(gstate.vertType, options);

	if (!Memory::IsValidRange(gstate_c.vertexAddr, (indexUpperBound + 1) * vdecoder.VertexSize()))
		return false;

	DrawEngineCommon::NormalizeVertices((u8 *)(&simpleVertices[0]), (u8 *)(&temp_buffer[0]), Memory::GetPointer(gstate_c.vertexAddr), &vdecoder, indexLowerBound, indexUpperBound, gstate.vertType);

	float world[16];
	float view[16];
	float worldview[16];
	float worldviewproj[16];
	ConvertMatrix4x3To4x4(world, gstate.worldMatrix);
	ConvertMatrix4x3To4x4(view, gstate.viewMatrix);
	Matrix4ByMatrix4(worldview, world, view);
	Matrix4ByMatrix4(worldviewproj, worldview, gstate.projMatrix);

	vertices.resize(indexUpperBound + 1);
	for (int i = indexLowerBound; i <= indexUpperBound; ++i) {
		const SimpleVertex &vert = simpleVertices[i];

		if (gstate.isModeThrough()) {
			if (gstate.vertType & GE_VTYPE_TC_MASK) {
				vertices[i].u = vert.uv[0];
				vertices[i].v = vert.uv[1];
			} else {
				vertices[i].u = 0.0f;
				vertices[i].v = 0.0f;
			}
			vertices[i].x = vert.pos.x;
			vertices[i].y = vert.pos.y;
			vertices[i].z = vert.pos.z;
		} else {
			float clipPos[4];
			Vec3ByMatrix44(clipPos, vert.pos.AsArray(), worldviewproj);
			ScreenCoords screenPos = ClipToScreen(clipPos);
			DrawingCoords drawPos = ScreenToDrawing(screenPos);

			if (gstate.vertType & GE_VTYPE_TC_MASK) {
				vertices[i].u = vert.uv[0] * (float)gstate.getTextureWidth(0);
				vertices[i].v = vert.uv[1] * (float)gstate.getTextureHeight(0);
			} else {
				vertices[i].u = 0.0f;
				vertices[i].v = 0.0f;
			}
			vertices[i].x = drawPos.x;
			vertices[i].y = drawPos.y;
			vertices[i].z = screenPos.z;
		}

		if (gstate.vertType & GE_VTYPE_COL_MASK) {
			memcpy(vertices[i].c, vert.color, sizeof(vertices[i].c));
		} else {
			memset(vertices[i].c, 0, sizeof(vertices[i].c));
		}
		vertices[i].nx = vert.nrm.x;
		vertices[i].ny = vert.nrm.y;
		vertices[i].nz = vert.nrm.z;
	}

	// The GE debugger expects these to be set.
	gstate_c.curTextureWidth = gstate.getTextureWidth(0);
	gstate_c.curTextureHeight = gstate.getTextureHeight(0);

	return true;
}
