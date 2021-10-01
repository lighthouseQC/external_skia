/*
 * Copyright 2018 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/android/SkAnimatedImage.h"
#include "include/codec/SkAndroidCodec.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkEncodedImageFormat.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkM44.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPathMeasure.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkScalar.h"
#include "include/core/SkShader.h"
#include "include/core/SkString.h"
#include "include/core/SkStrokeRec.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "include/core/SkVertices.h"
#include "include/effects/SkCornerPathEffect.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkDiscretePathEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkPerlinNoiseShader.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/effects/SkTrimPathEffect.h"
#include "include/private/SkShadowFlags.h"
#include "include/utils/SkParsePath.h"
#include "include/utils/SkShadowUtils.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "src/core/SkPathPriv.h"
#include "src/core/SkResourceCache.h"
#include "src/image/SkImage_Base.h"
#include "src/sksl/SkSLCompiler.h"

#include "modules/canvaskit/WasmCommon.h"
#include <emscripten.h>
#include <emscripten/bind.h>

#ifdef SK_GL
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/gl/GrGLInterface.h"
#include "include/gpu/gl/GrGLTypes.h"
#include "src/gpu/gl/GrGLDefines.h"

#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#endif

#ifndef SK_NO_FONTS
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontTypes.h"
#endif

#ifdef SK_INCLUDE_PATHOPS
#include "include/pathops/SkPathOps.h"
#endif

#ifndef SK_NO_FONTS
sk_sp<SkFontMgr> SkFontMgr_New_Custom_Data(sk_sp<SkData>* datas, int n);
#endif

struct OptionalMatrix : SkMatrix {
    OptionalMatrix(WASMPointerF32 mPtr) {
        if (mPtr) {
            const SkScalar* nineMatrixValues = reinterpret_cast<const SkScalar*>(mPtr);
            this->set9(nineMatrixValues);
        }
    }
};

SkColor4f ptrToSkColor4f(WASMPointerF32 cPtr) {
    float* fourFloats = reinterpret_cast<float*>(cPtr);
    SkColor4f color;
    memcpy(&color, fourFloats, 4 * sizeof(float));
    return color;
}

SkRRect ptrToSkRRect(WASMPointerF32 fPtr) {
    // In order, these floats should be 4 floats for the rectangle
    // (left, top, right, bottom) and then 8 floats for the radii
    // (upper left, upper right, lower right, lower left).
    const SkScalar* twelveFloats = reinterpret_cast<const SkScalar*>(fPtr);
    const SkRect rect = reinterpret_cast<const SkRect*>(twelveFloats)[0];
    const SkVector* radiiValues = reinterpret_cast<const SkVector*>(twelveFloats + 4);

    SkRRect rr;
    rr.setRectRadii(rect, radiiValues);
    return rr;
}

// Surface creation structs and helpers
struct SimpleImageInfo {
    int width;
    int height;
    SkColorType colorType;
    SkAlphaType alphaType;
    sk_sp<SkColorSpace> colorSpace;
};

SkImageInfo toSkImageInfo(const SimpleImageInfo& sii) {
    return SkImageInfo::Make(sii.width, sii.height, sii.colorType, sii.alphaType, sii.colorSpace);
}

#ifdef SK_GL

// Set the pixel format based on the colortype.
// These degrees of freedom are removed from canvaskit only to keep the interface simpler.
struct ColorSettings {
    ColorSettings(sk_sp<SkColorSpace> colorSpace) {
        if (colorSpace == nullptr || colorSpace->isSRGB()) {
            colorType = kRGBA_8888_SkColorType;
            pixFormat = GL_RGBA8;
        } else {
            colorType = kRGBA_F16_SkColorType;
            pixFormat = GL_RGBA16F;
        }
    };
    SkColorType colorType;
    GrGLenum pixFormat;
};

sk_sp<GrDirectContext> MakeGrContext(EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context)
{
    EMSCRIPTEN_RESULT r = emscripten_webgl_make_context_current(context);
    if (r < 0) {
        printf("failed to make webgl context current %d\n", r);
        return nullptr;
    }
    // setup interface
    auto interface = GrGLMakeNativeInterface();
    // setup context
    return GrDirectContext::MakeGL(interface);
}

sk_sp<SkSurface> MakeOnScreenGLSurface(sk_sp<GrDirectContext> dContext, int width, int height,
                                       sk_sp<SkColorSpace> colorSpace) {
    // WebGL should already be clearing the color and stencil buffers, but do it again here to
    // ensure Skia receives them in the expected state.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0, 0, 0, 0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    dContext->resetContext(kRenderTarget_GrGLBackendState | kMisc_GrGLBackendState);

    // The on-screen canvas is FBO 0. Wrap it in a Skia render target so Skia can render to it.
    GrGLFramebufferInfo info;
    info.fFBOID = 0;

    GrGLint sampleCnt;
    glGetIntegerv(GL_SAMPLES, &sampleCnt);

    GrGLint stencil;
    glGetIntegerv(GL_STENCIL_BITS, &stencil);

    const auto colorSettings = ColorSettings(colorSpace);
    info.fFormat = colorSettings.pixFormat;
    GrBackendRenderTarget target(width, height, sampleCnt, stencil, info);
    sk_sp<SkSurface> surface(SkSurface::MakeFromBackendRenderTarget(dContext.get(), target,
        kBottomLeft_GrSurfaceOrigin, colorSettings.colorType, colorSpace, nullptr));
    return surface;
}

sk_sp<SkSurface> MakeRenderTarget(sk_sp<GrDirectContext> dContext, int width, int height) {
    SkImageInfo info = SkImageInfo::MakeN32(width, height, SkAlphaType::kPremul_SkAlphaType);

    sk_sp<SkSurface> surface(SkSurface::MakeRenderTarget(dContext.get(),
                             SkBudgeted::kYes,
                             info, 0,
                             kBottomLeft_GrSurfaceOrigin,
                             nullptr, true));
    return surface;
}

sk_sp<SkSurface> MakeRenderTarget(sk_sp<GrDirectContext> dContext, SimpleImageInfo sii) {
    sk_sp<SkSurface> surface(SkSurface::MakeRenderTarget(dContext.get(),
                             SkBudgeted::kYes,
                             toSkImageInfo(sii), 0,
                             kBottomLeft_GrSurfaceOrigin,
                             nullptr, true));
    return surface;
}
#endif


//========================================================================================
// Path things
//========================================================================================

// All these Apply* methods are simple wrappers to avoid returning an object.
// The default WASM bindings produce code that will leak if a return value
// isn't assigned to a JS variable and has delete() called on it.
// These Apply methods, combined with the smarter binding code allow for chainable
// commands that don't leak if the return value is ignored (i.e. when used intuitively).
void ApplyAddPath(SkPath& orig, const SkPath& newPath,
                   SkScalar scaleX, SkScalar skewX,  SkScalar transX,
                   SkScalar skewY,  SkScalar scaleY, SkScalar transY,
                   SkScalar pers0, SkScalar pers1, SkScalar pers2,
                   bool extendPath) {
    SkMatrix m = SkMatrix::MakeAll(scaleX, skewX , transX,
                                   skewY , scaleY, transY,
                                   pers0 , pers1 , pers2);
    orig.addPath(newPath, m, extendPath ? SkPath::kExtend_AddPathMode :
                                          SkPath::kAppend_AddPathMode);
}

void ApplyArcToTangent(SkPath& p, SkScalar x1, SkScalar y1, SkScalar x2, SkScalar y2,
                SkScalar radius) {
    p.arcTo(x1, y1, x2, y2, radius);
}

void ApplyArcToArcSize(SkPath& orig, SkScalar rx, SkScalar ry, SkScalar xAxisRotate,
                       bool useSmallArc, bool ccw, SkScalar x, SkScalar y) {
    auto arcSize = useSmallArc ? SkPath::ArcSize::kSmall_ArcSize : SkPath::ArcSize::kLarge_ArcSize;
    auto sweep = ccw ? SkPathDirection::kCCW : SkPathDirection::kCW;
    orig.arcTo(rx, ry, xAxisRotate, arcSize, sweep, x, y);
}

void ApplyRArcToArcSize(SkPath& orig, SkScalar rx, SkScalar ry, SkScalar xAxisRotate,
                        bool useSmallArc, bool ccw, SkScalar dx, SkScalar dy) {
    auto arcSize = useSmallArc ? SkPath::ArcSize::kSmall_ArcSize : SkPath::ArcSize::kLarge_ArcSize;
    auto sweep = ccw ? SkPathDirection::kCCW : SkPathDirection::kCW;
    orig.rArcTo(rx, ry, xAxisRotate, arcSize, sweep, dx, dy);
}

void ApplyClose(SkPath& p) {
    p.close();
}

void ApplyConicTo(SkPath& p, SkScalar x1, SkScalar y1, SkScalar x2, SkScalar y2,
                  SkScalar w) {
    p.conicTo(x1, y1, x2, y2, w);
}

void ApplyRConicTo(SkPath& p, SkScalar dx1, SkScalar dy1, SkScalar dx2, SkScalar dy2,
                  SkScalar w) {
    p.rConicTo(dx1, dy1, dx2, dy2, w);
}

void ApplyCubicTo(SkPath& p, SkScalar x1, SkScalar y1, SkScalar x2, SkScalar y2,
                  SkScalar x3, SkScalar y3) {
    p.cubicTo(x1, y1, x2, y2, x3, y3);
}

void ApplyRCubicTo(SkPath& p, SkScalar dx1, SkScalar dy1, SkScalar dx2, SkScalar dy2,
                  SkScalar dx3, SkScalar dy3) {
    p.rCubicTo(dx1, dy1, dx2, dy2, dx3, dy3);
}

void ApplyLineTo(SkPath& p, SkScalar x, SkScalar y) {
    p.lineTo(x, y);
}

void ApplyRLineTo(SkPath& p, SkScalar dx, SkScalar dy) {
    p.rLineTo(dx, dy);
}

void ApplyMoveTo(SkPath& p, SkScalar x, SkScalar y) {
    p.moveTo(x, y);
}

void ApplyRMoveTo(SkPath& p, SkScalar dx, SkScalar dy) {
    p.rMoveTo(dx, dy);
}

void ApplyReset(SkPath& p) {
    p.reset();
}

void ApplyRewind(SkPath& p) {
    p.rewind();
}

void ApplyQuadTo(SkPath& p, SkScalar x1, SkScalar y1, SkScalar x2, SkScalar y2) {
    p.quadTo(x1, y1, x2, y2);
}

void ApplyRQuadTo(SkPath& p, SkScalar dx1, SkScalar dy1, SkScalar dx2, SkScalar dy2) {
    p.rQuadTo(dx1, dy1, dx2, dy2);
}

void ApplyTransform(SkPath& orig,
                    SkScalar scaleX, SkScalar skewX,  SkScalar transX,
                    SkScalar skewY,  SkScalar scaleY, SkScalar transY,
                    SkScalar pers0, SkScalar pers1, SkScalar pers2) {
    SkMatrix m = SkMatrix::MakeAll(scaleX, skewX , transX,
                                   skewY , scaleY, transY,
                                   pers0 , pers1 , pers2);
    orig.transform(m);
}

#ifdef SK_INCLUDE_PATHOPS
bool ApplySimplify(SkPath& path) {
    return Simplify(path, &path);
}

bool ApplyPathOp(SkPath& pathOne, const SkPath& pathTwo, SkPathOp op) {
    return Op(pathOne, pathTwo, op, &pathOne);
}

SkPathOrNull MakePathFromOp(const SkPath& pathOne, const SkPath& pathTwo, SkPathOp op) {
    SkPath out;
    if (Op(pathOne, pathTwo, op, &out)) {
        return emscripten::val(out);
    }
    return emscripten::val::null();
}

SkPathOrNull MakeAsWinding(const SkPath& self) {
    SkPath out;
    if (AsWinding(self, &out)) {
        return emscripten::val(out);
    }
    return emscripten::val::null();
}
#endif

JSString ToSVGString(const SkPath& path) {
    SkString s;
    SkParsePath::ToSVGString(path, &s);
    return emscripten::val(s.c_str());
}

SkPathOrNull MakePathFromSVGString(std::string str) {
    SkPath path;
    if (SkParsePath::FromSVGString(str.c_str(), &path)) {
        return emscripten::val(path);
    }
    return emscripten::val::null();
}

SkPath CopyPath(const SkPath& a) {
    SkPath copy(a);
    return copy;
}

bool Equals(const SkPath& a, const SkPath& b) {
    return a == b;
}

// =================================================================================
// Creating/Exporting Paths with cmd arrays
// =================================================================================

static const int MOVE = 0;
static const int LINE = 1;
static const int QUAD = 2;
static const int CONIC = 3;
static const int CUBIC = 4;
static const int CLOSE = 5;

Float32Array ToCmds(const SkPath& path) {
    std::vector<SkScalar> cmds;
    for (auto [verb, pts, w] : SkPathPriv::Iterate(path)) {
        switch (verb) {
        case SkPathVerb::kMove:
            cmds.insert(cmds.end(), {MOVE, pts[0].x(), pts[0].y()});
            break;
        case SkPathVerb::kLine:
            cmds.insert(cmds.end(), {LINE, pts[1].x(), pts[1].y()});
            break;
        case SkPathVerb::kQuad:
            cmds.insert(cmds.end(), {QUAD, pts[1].x(), pts[1].y(), pts[2].x(), pts[2].y()});
            break;
        case SkPathVerb::kConic:
            cmds.insert(cmds.end(), {CONIC,
                           pts[1].x(), pts[1].y(),
                           pts[2].x(), pts[2].y(), *w});
            break;
        case SkPathVerb::kCubic:
            cmds.insert(cmds.end(), {CUBIC,
                           pts[1].x(), pts[1].y(),
                           pts[2].x(), pts[2].y(),
                           pts[3].x(), pts[3].y()});
            break;
        case SkPathVerb::kClose:
            cmds.push_back(CLOSE);
            break;
        }
    }
    return MakeTypedArray(cmds.size(), (const float*)cmds.data());
}

SkPathOrNull MakePathFromCmds(WASMPointerF32 cptr, int numCmds) {
    const auto* cmds = reinterpret_cast<const float*>(cptr);
    SkPath path;
    float x1, y1, x2, y2, x3, y3;

    // if there are not enough arguments, bail with the path we've constructed so far.
    #define CHECK_NUM_ARGS(n) \
        if ((i + n) > numCmds) { \
            SkDebugf("Not enough args to match the verbs. Saw %d commands\n", numCmds); \
            return emscripten::val::null(); \
        }

    for(int i = 0; i < numCmds;){
         switch (sk_float_floor2int(cmds[i++])) {
            case MOVE:
                CHECK_NUM_ARGS(2);
                x1 = cmds[i++], y1 = cmds[i++];
                path.moveTo(x1, y1);
                break;
            case LINE:
                CHECK_NUM_ARGS(2);
                x1 = cmds[i++], y1 = cmds[i++];
                path.lineTo(x1, y1);
                break;
            case QUAD:
                CHECK_NUM_ARGS(4);
                x1 = cmds[i++], y1 = cmds[i++];
                x2 = cmds[i++], y2 = cmds[i++];
                path.quadTo(x1, y1, x2, y2);
                break;
            case CONIC:
                CHECK_NUM_ARGS(5);
                x1 = cmds[i++], y1 = cmds[i++];
                x2 = cmds[i++], y2 = cmds[i++];
                x3 = cmds[i++]; // weight
                path.conicTo(x1, y1, x2, y2, x3);
                break;
            case CUBIC:
                CHECK_NUM_ARGS(6);
                x1 = cmds[i++], y1 = cmds[i++];
                x2 = cmds[i++], y2 = cmds[i++];
                x3 = cmds[i++], y3 = cmds[i++];
                path.cubicTo(x1, y1, x2, y2, x3, y3);
                break;
            case CLOSE:
                path.close();
                break;
            default:
                SkDebugf("  path: UNKNOWN command %f, aborting dump...\n", cmds[i-1]);
                return emscripten::val::null();
        }
    }

    #undef CHECK_NUM_ARGS

    return emscripten::val(path);
}

void PathAddVerbsPointsWeights(SkPath& path, WASMPointerU8 verbsPtr, int numVerbs,
                                             WASMPointerF32 ptsPtr, int numPts,
                                             WASMPointerF32 wtsPtr, int numWts) {
    const uint8_t* verbs = reinterpret_cast<const uint8_t*>(verbsPtr);
    const float* pts = reinterpret_cast<const float*>(ptsPtr);
    const float* weights = reinterpret_cast<const float*>(wtsPtr);

    #define CHECK_NUM_POINTS(n) \
        if ((ptIdx + n) > numPts) { \
            SkDebugf("Not enough points to match the verbs. Saw %d points\n", numPts); \
            return; \
        }
    #define CHECK_NUM_WEIGHTS(n) \
        if ((wtIdx + n) > numWts) { \
            SkDebugf("Not enough weights to match the verbs. Saw %d weights\n", numWts); \
            return; \
        }

    path.incReserve(numPts);
    int ptIdx = 0;
    int wtIdx = 0;
    for (int v = 0; v < numVerbs; ++v) {
         switch (verbs[v]) {
              case MOVE:
                  CHECK_NUM_POINTS(2);
                  path.moveTo(pts[ptIdx], pts[ptIdx+1]);
                  ptIdx += 2;
                  break;
              case LINE:
                  CHECK_NUM_POINTS(2);
                  path.lineTo(pts[ptIdx], pts[ptIdx+1]);
                  ptIdx += 2;
                  break;
              case QUAD:
                  CHECK_NUM_POINTS(4);
                  path.quadTo(pts[ptIdx], pts[ptIdx+1], pts[ptIdx+2], pts[ptIdx+3]);
                  ptIdx += 4;
                  break;
              case CONIC:
                  CHECK_NUM_POINTS(4);
                  CHECK_NUM_WEIGHTS(1);
                  path.conicTo(pts[ptIdx], pts[ptIdx+1], pts[ptIdx+2], pts[ptIdx+3],
                               weights[wtIdx]);
                  ptIdx += 4;
                  wtIdx++;
                  break;
              case CUBIC:
                  CHECK_NUM_POINTS(6);
                  path.cubicTo(pts[ptIdx  ], pts[ptIdx+1],
                               pts[ptIdx+2], pts[ptIdx+3],
                               pts[ptIdx+4], pts[ptIdx+5]);
                  ptIdx += 6;
                  break;
              case CLOSE:
                  path.close();
                  break;
        }
    }
    #undef CHECK_NUM_POINTS
    #undef CHECK_NUM_WEIGHTS
}

SkPath MakePathFromVerbsPointsWeights(WASMPointerU8 verbsPtr, int numVerbs,
                                      WASMPointerF32 ptsPtr, int numPts,
                                      WASMPointerF32 wtsPtr, int numWts) {
    SkPath path;
    PathAddVerbsPointsWeights(path, verbsPtr, numVerbs, ptsPtr, numPts, wtsPtr, numWts);
    return path;
}

//========================================================================================
// Path Effects
//========================================================================================

bool ApplyDash(SkPath& path, SkScalar on, SkScalar off, SkScalar phase) {
    SkScalar intervals[] = { on, off };
    auto pe = SkDashPathEffect::Make(intervals, 2, phase);
    if (!pe) {
        SkDebugf("Invalid args to dash()\n");
        return false;
    }
    SkStrokeRec rec(SkStrokeRec::InitStyle::kHairline_InitStyle);
    if (pe->filterPath(&path, path, &rec, nullptr)) {
        return true;
    }
    SkDebugf("Could not make dashed path\n");
    return false;
}

bool ApplyTrim(SkPath& path, SkScalar startT, SkScalar stopT, bool isComplement) {
    auto mode = isComplement ? SkTrimPathEffect::Mode::kInverted : SkTrimPathEffect::Mode::kNormal;
    auto pe = SkTrimPathEffect::Make(startT, stopT, mode);
    if (!pe) {
        SkDebugf("Invalid args to trim(): startT and stopT must be in [0,1]\n");
        return false;
    }
    SkStrokeRec rec(SkStrokeRec::InitStyle::kHairline_InitStyle);
    if (pe->filterPath(&path, path, &rec, nullptr)) {
        return true;
    }
    SkDebugf("Could not trim path\n");
    return false;
}

struct StrokeOpts {
    // Default values are set in interface.js which allows clients
    // to set any number of them. Otherwise, the binding code complains if
    // any are omitted.
    SkScalar width;
    SkScalar miter_limit;
    SkPaint::Join join;
    SkPaint::Cap cap;
    float precision;
};

bool ApplyStroke(SkPath& path, StrokeOpts opts) {
    SkPaint p;
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeCap(opts.cap);
    p.setStrokeJoin(opts.join);
    p.setStrokeWidth(opts.width);
    p.setStrokeMiter(opts.miter_limit);

    return p.getFillPath(path, &path, nullptr, opts.precision);
}

// This function is private, we call it in interface.js
void computeTonalColors(WASMPointerF32 cPtrAmbi, WASMPointerF32 cPtrSpot) {
    // private methods accepting colors take pointers to floats already copied into wasm memory.
    float* ambiFloats = reinterpret_cast<float*>(cPtrAmbi);
    float* spotFloats = reinterpret_cast<float*>(cPtrSpot);
    SkColor4f ambiColor = { ambiFloats[0], ambiFloats[1], ambiFloats[2], ambiFloats[3]};
    SkColor4f spotColor = { spotFloats[0], spotFloats[1], spotFloats[2], spotFloats[3]};

    // This function takes SkColor
    SkColor resultAmbi, resultSpot;
    SkShadowUtils::ComputeTonalColors(
        ambiColor.toSkColor(), spotColor.toSkColor(),
        &resultAmbi, &resultSpot);

    // Convert back to color4f
    const SkColor4f ambi4f = SkColor4f::FromColor(resultAmbi);
    const SkColor4f spot4f = SkColor4f::FromColor(resultSpot);

    // Re-use the caller's allocated memory to hold the result.
    memcpy(ambiFloats, ambi4f.vec(), 4 * sizeof(SkScalar));
    memcpy(spotFloats, spot4f.vec(), 4 * sizeof(SkScalar));
}

#ifdef SK_INCLUDE_RUNTIME_EFFECT
struct RuntimeEffectUniform {
    int columns;
    int rows;
    int slot; // the index into the uniforms array that this uniform begins.
    bool isInteger;
};

RuntimeEffectUniform fromUniform(const SkRuntimeEffect::Uniform& u) {
    RuntimeEffectUniform su;
    su.rows      = u.count;  // arrayLength
    su.columns   = 1;
    su.isInteger = false;
    using Type = SkRuntimeEffect::Uniform::Type;
    switch (u.type) {
        case Type::kFloat:                                                       break;
        case Type::kFloat2:   su.columns = 2;                                    break;
        case Type::kFloat3:   su.columns = 3;                                    break;
        case Type::kFloat4:   su.columns = 4;                                    break;
        case Type::kFloat2x2: su.columns = 2; su.rows *= 2;                      break;
        case Type::kFloat3x3: su.columns = 3; su.rows *= 3;                      break;
        case Type::kFloat4x4: su.columns = 4; su.rows *= 4;                      break;
        case Type::kInt:                                    su.isInteger = true; break;
        case Type::kInt2:     su.columns = 2;               su.isInteger = true; break;
        case Type::kInt3:     su.columns = 3;               su.isInteger = true; break;
        case Type::kInt4:     su.columns = 4;               su.isInteger = true; break;
    }
    su.slot = u.offset / sizeof(float);
    return su;
}

void castUniforms(void* data, size_t dataLen, const SkRuntimeEffect& effect) {
    if (dataLen != effect.uniformSize()) {
        // Incorrect number of uniforms. Our code below could read/write off the end of the buffer.
        // However, shader creation is going to fail anyway, so just do nothing.
        return;
    }

    float* fltData = reinterpret_cast<float*>(data);
    for (const auto& u : effect.uniforms()) {
        RuntimeEffectUniform reu = fromUniform(u);
        if (reu.isInteger) {
            // The SkSL is expecting integers in the uniform data
            for (int i = 0; i < reu.columns * reu.rows; ++i) {
                int numAsInt = static_cast<int>(fltData[reu.slot + i]);
                fltData[reu.slot + i] = SkBits2Float(numAsInt);
            }
        }
    }
}
#endif

// These objects have private destructors / delete methods - I don't think
// we need to do anything other than tell emscripten to do nothing.
namespace emscripten {
    namespace internal {
        template<typename ClassType>
        void raw_destructor(ClassType *);

        template<>
        void raw_destructor<SkContourMeasure>(SkContourMeasure *ptr) {
        }

        template<>
        void raw_destructor<SkVertices>(SkVertices *ptr) {
        }

#ifndef SK_NO_FONTS
        template<>
        void raw_destructor<SkTextBlob>(SkTextBlob *ptr) {
        }

        template<>
        void raw_destructor<SkTypeface>(SkTypeface *ptr) {
        }
#endif
    }
}

// toBytes returns a Uint8Array that has a copy of the data in the given SkData.
Uint8Array toBytes(sk_sp<SkData> data) {
    // By making the copy using the JS transliteration, we don't risk the SkData object being
    // cleaned up before we make the copy.
    return emscripten::val(
        // https://emscripten.org/docs/porting/connecting_cpp_and_javascript/embind.html#memory-views
        typed_memory_view(data->size(), data->bytes())
    ).call<Uint8Array>("slice"); // slice with no args makes a copy of the memory view.
}

// We need to call into the JS side of things to free webGL contexts. This object will be called
// with _setTextureCleanup after CanvasKit loads. The object will have one attribute,
// a function called deleteTexture that takes two ints.
JSObject textureCleanup = emscripten::val::null();

struct TextureReleaseContext {
    uint32_t webglHandle;
    uint32_t texHandle;
};

void deleteJSTexture(SkImage::ReleaseContext rc) {
    auto ctx = reinterpret_cast<TextureReleaseContext*>(rc);
    textureCleanup.call<void>("deleteTexture", ctx->webglHandle, ctx->texHandle);
    delete ctx;
}

EMSCRIPTEN_BINDINGS(Skia) {
#ifdef SK_GL
    function("currentContext", &emscripten_webgl_get_current_context);
    function("setCurrentContext", &emscripten_webgl_make_context_current);
    function("MakeGrContext", &MakeGrContext);
    function("MakeOnScreenGLSurface", &MakeOnScreenGLSurface);
    function("MakeRenderTarget", select_overload<sk_sp<SkSurface>(sk_sp<GrDirectContext>, int, int)>(&MakeRenderTarget));
    function("MakeRenderTarget", select_overload<sk_sp<SkSurface>(sk_sp<GrDirectContext>, SimpleImageInfo)>(&MakeRenderTarget));

    constant("gpu", true);
#endif
    function("getDecodeCacheLimitBytes", &SkResourceCache::GetTotalByteLimit);
    function("setDecodeCacheLimitBytes", &SkResourceCache::SetTotalByteLimit);
    function("getDecodeCacheUsedBytes" , &SkResourceCache::GetTotalBytesUsed);

    function("_computeTonalColors", &computeTonalColors);
    function("_decodeAnimatedImage", optional_override([](WASMPointerU8 iptr,
                                                  size_t length)->sk_sp<SkAnimatedImage> {
        uint8_t* imgData = reinterpret_cast<uint8_t*>(iptr);
        auto bytes = SkData::MakeFromMalloc(imgData, length);
        auto aCodec = SkAndroidCodec::MakeFromData(std::move(bytes));
        if (nullptr == aCodec) {
            return nullptr;
        }

        return SkAnimatedImage::Make(std::move(aCodec));
    }), allow_raw_pointers());
    function("_decodeImage", optional_override([](WASMPointerU8 iptr,
                                                  size_t length)->sk_sp<SkImage> {
        uint8_t* imgData = reinterpret_cast<uint8_t*>(iptr);
        sk_sp<SkData> bytes = SkData::MakeFromMalloc(imgData, length);
        return SkImage::MakeFromEncoded(std::move(bytes));
    }), allow_raw_pointers());

    // These won't be called directly, there are corresponding JS helpers to deal with arrays.
    function("_MakeImage", optional_override([](SimpleImageInfo ii,
                                                WASMPointerU8 pPtr, int plen,
                                                size_t rowBytes)->sk_sp<SkImage> {
        uint8_t* pixels = reinterpret_cast<uint8_t*>(pPtr);
        SkImageInfo info = toSkImageInfo(ii);
        sk_sp<SkData> pixelData = SkData::MakeFromMalloc(pixels, plen);

        return SkImage::MakeRasterData(info, pixelData, rowBytes);
    }), allow_raw_pointers());

    function("_getShadowLocalBounds", optional_override([](
            WASMPointerF32 ctmPtr, const SkPath& path,
            WASMPointerF32  zPlaneParamPtr, WASMPointerF32 lightPosPtr,
            SkScalar lightRadius, uint32_t flags, WASMPointerF32 outPtr) -> bool {
        SkMatrix ctm;
        const SkScalar* nineMatrixValues = reinterpret_cast<const SkScalar*>(ctmPtr);
        ctm.set9(nineMatrixValues);
        const SkVector3* zPlaneParams = reinterpret_cast<const SkVector3*>(zPlaneParamPtr);
        const SkVector3* lightPos = reinterpret_cast<const SkVector3*>(lightPosPtr);
        SkRect* outputBounds = reinterpret_cast<SkRect*>(outPtr);
        return SkShadowUtils::GetLocalBounds(ctm, path, *zPlaneParams, *lightPos, lightRadius,
                              flags, outputBounds);
    }));

#ifdef SK_SERIALIZE_SKP
    function("_MakePicture", optional_override([](WASMPointerU8 dPtr,
                                                  size_t bytes)->sk_sp<SkPicture> {
        uint8_t* d = reinterpret_cast<uint8_t*>(dPtr);
        sk_sp<SkData> data = SkData::MakeFromMalloc(d, bytes);

        return SkPicture::MakeFromData(data.get(), nullptr);
    }), allow_raw_pointers());
#endif

#ifdef SK_GL
    class_<GrDirectContext>("GrDirectContext")
        .smart_ptr<sk_sp<GrDirectContext>>("sk_sp<GrDirectContext>")
        .function("getResourceCacheLimitBytes",
                optional_override([](GrDirectContext& self)->size_t {
            int maxResources = 0;// ignored
            size_t currMax = 0;
            self.getResourceCacheLimits(&maxResources, &currMax);
            return currMax;
        }))
        .function("getResourceCacheUsageBytes",
                optional_override([](GrDirectContext& self)->size_t {
            int usedResources = 0;// ignored
            size_t currUsage = 0;
            self.getResourceCacheUsage(&usedResources, &currUsage);
            return currUsage;
        }))
        .function("releaseResourcesAndAbandonContext",
                &GrDirectContext::releaseResourcesAndAbandonContext)
        .function("setResourceCacheLimitBytes",
                optional_override([](GrDirectContext& self, size_t maxResourceBytes)->void {
            int maxResources = 0;
            size_t currMax = 0; // ignored
            self.getResourceCacheLimits(&maxResources, &currMax);
            self.setResourceCacheLimits(maxResources, maxResourceBytes);
        }));

    // This allows us to give the C++ code a JS callback to delete textures that
    // have been passed in via makeImageFromTexture and makeImageFromTextureSource.
    function("_setTextureCleanup", optional_override([](JSObject callbackObj)->void {
         textureCleanup = callbackObj;
     }));
#endif

    class_<SkAnimatedImage>("AnimatedImage")
        .smart_ptr<sk_sp<SkAnimatedImage>>("sk_sp<AnimatedImage>")
        .function("decodeNextFrame", &SkAnimatedImage::decodeNextFrame)
        .function("getFrameCount", &SkAnimatedImage::getFrameCount)
        .function("getRepetitionCount", &SkAnimatedImage::getRepetitionCount)
        .function("height",  optional_override([](SkAnimatedImage& self)->int32_t {
            // getBounds returns an SkRect, but internally, the width and height are ints.
            return SkScalarFloorToInt(self.getBounds().height());
        }))
        .function("makeImageAtCurrentFrame", &SkAnimatedImage::getCurrentFrame)
        .function("reset", &SkAnimatedImage::reset)
        .function("width",  optional_override([](SkAnimatedImage& self)->int32_t {
            return SkScalarFloorToInt(self.getBounds().width());
        }));

    class_<SkCanvas>("Canvas")
        .constructor<>()
        .function("_clear", optional_override([](SkCanvas& self, WASMPointerF32 cPtr) {
            self.clear(ptrToSkColor4f(cPtr));
        }))
        .function("clipPath", select_overload<void (const SkPath&, SkClipOp, bool)>(&SkCanvas::clipPath))
        .function("_clipRRect", optional_override([](SkCanvas& self, WASMPointerF32 fPtr, SkClipOp op, bool doAntiAlias) {
            self.clipRRect(ptrToSkRRect(fPtr), op, doAntiAlias);
        }))
        .function("_clipRect", optional_override([](SkCanvas& self, WASMPointerF32 fPtr, SkClipOp op, bool doAntiAlias) {
            const SkRect* rect = reinterpret_cast<const SkRect*>(fPtr);
            self.clipRect(*rect, op, doAntiAlias);
        }))
        .function("_concat", optional_override([](SkCanvas& self, WASMPointerF32 mPtr) {
            //TODO(skbug.com/10108): make the JS side be column major.
            const SkScalar* sixteenMatrixValues = reinterpret_cast<const SkScalar*>(mPtr);
            SkM44 m = SkM44::RowMajor(sixteenMatrixValues);
            self.concat(m);
        }))
        .function("_drawArc", optional_override([](SkCanvas& self, WASMPointerF32 fPtr,
                                                  SkScalar startAngle, SkScalar sweepAngle,
                                                  bool useCenter, const SkPaint& paint) {
            const SkRect* oval = reinterpret_cast<const SkRect*>(fPtr);
            self.drawArc(*oval, startAngle, sweepAngle, useCenter, paint);
        }))
        .function("_drawAtlasOptions", optional_override([](SkCanvas& self,
                const sk_sp<SkImage>& atlas, WASMPointerF32 xptr,
                WASMPointerF32 rptr, WASMPointerU32 cptr, int count,
                SkBlendMode mode, SkFilterMode filter, SkMipmapMode mipmap,
                const SkPaint* paint)->void {
            const SkRSXform* dstXforms = reinterpret_cast<const SkRSXform*>(xptr);
            const SkRect* srcRects = reinterpret_cast<const SkRect*>(rptr);
            const SkColor* colors = nullptr;
            if (cptr) {
                colors = reinterpret_cast<const SkColor*>(cptr);
            }
            SkSamplingOptions sampling(filter, mipmap);
            self.drawAtlas(atlas.get(), dstXforms, srcRects, colors, count, mode, sampling,
                           nullptr, paint);
        }), allow_raw_pointers())
        .function("_drawAtlasCubic", optional_override([](SkCanvas& self,
                const sk_sp<SkImage>& atlas, WASMPointerF32 xptr,
                WASMPointerF32 rptr, WASMPointerU32 cptr, int count,
                SkBlendMode mode, float B, float C, const SkPaint* paint)->void {
            const SkRSXform* dstXforms = reinterpret_cast<const SkRSXform*>(xptr);
            const SkRect* srcRects = reinterpret_cast<const SkRect*>(rptr);
            const SkColor* colors = nullptr;
            if (cptr) {
                colors = reinterpret_cast<const SkColor*>(cptr);
            }
            SkSamplingOptions sampling({B, C});
            self.drawAtlas(atlas.get(), dstXforms, srcRects, colors, count, mode, sampling,
                           nullptr, paint);
        }), allow_raw_pointers())
        .function("drawCircle", select_overload<void (SkScalar, SkScalar, SkScalar, const SkPaint& paint)>(&SkCanvas::drawCircle))
        .function("_drawColor", optional_override([](SkCanvas& self, WASMPointerF32 cPtr) {
            self.drawColor(ptrToSkColor4f(cPtr));
        }))
        .function("_drawColor", optional_override([](SkCanvas& self, WASMPointerF32 cPtr, SkBlendMode mode) {
            self.drawColor(ptrToSkColor4f(cPtr), mode);
        }))
        .function("drawColorInt", optional_override([](SkCanvas& self, SkColor color) {
            self.drawColor(color);
        }))
        .function("drawColorInt", optional_override([](SkCanvas& self, SkColor color, SkBlendMode mode) {
            self.drawColor(color, mode);
        }))
        .function("_drawDRRect", optional_override([](SkCanvas& self, WASMPointerF32 outerPtr,
                                                     WASMPointerF32 innerPtr, const SkPaint& paint) {
            self.drawDRRect(ptrToSkRRect(outerPtr), ptrToSkRRect(innerPtr), paint);
        }))
        .function("_drawGlyphs", optional_override([](SkCanvas& self,
                                                      int count,
                                                      WASMPointerU16 glyphs,
                                                      WASMPointerF32 positions,
                                                      float x, float y,
                                                      const SkFont& font,
                                                      const SkPaint& paint)->void {
            self.drawGlyphs(count,
                            reinterpret_cast<const uint16_t*>(glyphs),
                            reinterpret_cast<const SkPoint*>(positions),
                            {x, y}, font, paint);
        }))
        // TODO: deprecate this version, and require sampling
        .function("drawImage", optional_override([](SkCanvas& self, const sk_sp<SkImage>& image,
                                                    SkScalar x, SkScalar y, const SkPaint* paint) {
            self.drawImage(image.get(), x, y, SkSamplingOptions(), paint);
        }), allow_raw_pointers())
        .function("drawImageCubic",  optional_override([](SkCanvas& self, const sk_sp<SkImage>& img,
                                                          SkScalar left, SkScalar top,
                                                          float B, float C, // See SkSamplingOptions.h for docs.
                                                          const SkPaint* paint)->void {
            self.drawImage(img.get(), left, top, SkSamplingOptions({B, C}), paint);
        }), allow_raw_pointers())
        .function("drawImageOptions",  optional_override([](SkCanvas& self, const sk_sp<SkImage>& img,
                                                          SkScalar left, SkScalar top,
                                                          SkFilterMode filter, SkMipmapMode mipmap,
                                                          const SkPaint* paint)->void {
            self.drawImage(img.get(), left, top, {filter, mipmap}, paint);
        }), allow_raw_pointers())

        .function("_drawImageNine", optional_override([](SkCanvas& self, const sk_sp<SkImage>& image,
                                                         WASMPointerU32 centerPtr, WASMPointerF32 dstPtr,
                                                         SkFilterMode filter, const SkPaint* paint)->void {
            const SkIRect* center = reinterpret_cast<const SkIRect*>(centerPtr);
            const SkRect* dst = reinterpret_cast<const SkRect*>(dstPtr);

            self.drawImageNine(image.get(), *center, *dst, filter, paint);
        }), allow_raw_pointers())
        // TODO: deprecate this version, and require sampling
        .function("_drawImageRect", optional_override([](SkCanvas& self, const sk_sp<SkImage>& image,
                                                         WASMPointerF32 srcPtr, WASMPointerF32 dstPtr,
                                                         const SkPaint* paint, bool fastSample)->void {
            const SkRect* src = reinterpret_cast<const SkRect*>(srcPtr);
            const SkRect* dst = reinterpret_cast<const SkRect*>(dstPtr);
            self.drawImageRect(image, *src, *dst, SkSamplingOptions(), paint,
                               fastSample ? SkCanvas::kFast_SrcRectConstraint:
                                            SkCanvas::kStrict_SrcRectConstraint);
        }), allow_raw_pointers())
        .function("_drawImageRectCubic", optional_override([](SkCanvas& self, const sk_sp<SkImage>& image,
                                                              WASMPointerF32 srcPtr, WASMPointerF32 dstPtr,
                                                              float B, float C, // See SkSamplingOptions.h for docs.
                                                              const SkPaint* paint)->void {
            const SkRect* src = reinterpret_cast<const SkRect*>(srcPtr);
            const SkRect* dst = reinterpret_cast<const SkRect*>(dstPtr);
            auto constraint = SkCanvas::kStrict_SrcRectConstraint;  // TODO: get from caller
            self.drawImageRect(image.get(), *src, *dst, SkSamplingOptions({B, C}), paint, constraint);
        }), allow_raw_pointers())
        .function("_drawImageRectOptions", optional_override([](SkCanvas& self, const sk_sp<SkImage>& image,
                                                                WASMPointerF32 srcPtr, WASMPointerF32 dstPtr,
                                                                SkFilterMode filter, SkMipmapMode mipmap,
                                                                const SkPaint* paint)->void {
            const SkRect* src = reinterpret_cast<const SkRect*>(srcPtr);
            const SkRect* dst = reinterpret_cast<const SkRect*>(dstPtr);
            auto constraint = SkCanvas::kStrict_SrcRectConstraint;  // TODO: get from caller
            self.drawImageRect(image.get(), *src, *dst, {filter, mipmap}, paint, constraint);
        }), allow_raw_pointers())
        .function("drawLine", select_overload<void (SkScalar, SkScalar, SkScalar, SkScalar, const SkPaint&)>(&SkCanvas::drawLine))
        .function("_drawOval", optional_override([](SkCanvas& self, WASMPointerF32 fPtr,
                                                    const SkPaint& paint)->void {
            const SkRect* oval = reinterpret_cast<const SkRect*>(fPtr);
            self.drawOval(*oval, paint);
        }))
        .function("drawPaint", &SkCanvas::drawPaint)
#ifdef SK_INCLUDE_PARAGRAPH
        .function("drawParagraph", optional_override([](SkCanvas& self, skia::textlayout::Paragraph* p,
                                                        SkScalar x, SkScalar y) {
            p->paint(&self, x, y);
        }), allow_raw_pointers())
#endif
        .function("drawPath", &SkCanvas::drawPath)
        .function("_drawPatch", optional_override([](SkCanvas& self,
                                                     WASMPointerF32 cubics,
                                                     WASMPointerU32 colors,
                                                     WASMPointerF32 texs,
                                                     SkBlendMode mode,
                                                     const SkPaint& paint)->void {
            self.drawPatch(reinterpret_cast<const SkPoint*>(cubics),
                           reinterpret_cast<const SkColor*>(colors),
                           reinterpret_cast<const SkPoint*>(texs),
                           mode, paint);
        }))
        // Of note, picture is *not* what is colloquially thought of as a "picture", what we call
        // a bitmap. An SkPicture is a series of draw commands.
        .function("drawPicture", select_overload<void (const sk_sp<SkPicture>&)>(&SkCanvas::drawPicture))
        .function("_drawPoints", optional_override([](SkCanvas& self, SkCanvas::PointMode mode,
                                                     WASMPointerF32 pptr,
                                                     int count, SkPaint& paint)->void {
            const SkPoint* pts = reinterpret_cast<const SkPoint*>(pptr);
            self.drawPoints(mode, count, pts, paint);
        }))
        .function("_drawRRect",optional_override([](SkCanvas& self, WASMPointerF32 fPtr, const SkPaint& paint) {
            self.drawRRect(ptrToSkRRect(fPtr), paint);
        }))
        .function("_drawRect", optional_override([](SkCanvas& self, WASMPointerF32 fPtr,
                                                    const SkPaint& paint)->void {
            const SkRect* rect = reinterpret_cast<const SkRect*>(fPtr);
            self.drawRect(*rect, paint);
        }))
        .function("drawRect4f", optional_override([](SkCanvas& self, SkScalar left, SkScalar top,
                                                     SkScalar right, SkScalar bottom,
                                                     const SkPaint& paint)->void {
            const SkRect rect = SkRect::MakeLTRB(left, top, right, bottom);
            self.drawRect(rect, paint);
        }))
        .function("_drawShadow", optional_override([](SkCanvas& self, const SkPath& path,
                                                     WASMPointerF32 zPlaneParamPtr,
                                                     WASMPointerF32 lightPosPtr,
                                                     SkScalar lightRadius,
                                                     WASMPointerF32 ambientColorPtr,
                                                     WASMPointerF32 spotColorPtr,
                                                     uint32_t flags) {
            const SkVector3* zPlaneParams = reinterpret_cast<const SkVector3*>(zPlaneParamPtr);
            const SkVector3* lightPos = reinterpret_cast<const SkVector3*>(lightPosPtr);

            SkShadowUtils::DrawShadow(&self, path, *zPlaneParams, *lightPos, lightRadius,
                                      ptrToSkColor4f(ambientColorPtr).toSkColor(),
                                      ptrToSkColor4f(spotColorPtr).toSkColor(),
                                      flags);
        }))
#ifndef SK_NO_FONTS
        .function("_drawSimpleText", optional_override([](SkCanvas& self, WASMPointerU8 sptr,
                                                          size_t len, SkScalar x, SkScalar y, const SkFont& font,
                                                          const SkPaint& paint) {
            const char* str = reinterpret_cast<const char*>(sptr);

            self.drawSimpleText(str, len, SkTextEncoding::kUTF8, x, y, font, paint);
        }))
        .function("drawTextBlob", select_overload<void (const sk_sp<SkTextBlob>&, SkScalar, SkScalar, const SkPaint&)>(&SkCanvas::drawTextBlob))
#endif
        .function("drawVertices", select_overload<void (const sk_sp<SkVertices>&, SkBlendMode, const SkPaint&)>(&SkCanvas::drawVertices))
        .function("_findMarkedCTM", optional_override([](SkCanvas& self, std::string marker, WASMPointerF32 mPtr) -> bool {
            SkScalar* sixteenMatrixValues = reinterpret_cast<SkScalar*>(mPtr);
            if (!sixteenMatrixValues) {
                return false; // matrix cannot be null
            }
            SkM44 m;
            if (self.findMarkedCTM(marker.c_str(), &m)) {
                m.getRowMajor(sixteenMatrixValues);
                return true;
            }
            return false;
        }))
        // 4x4 matrix functions
        // Just like with getTotalMatrix, we allocate the buffer for the 16 floats to go in from
        // interface.js, so it can also free them when its done.
        .function("_getLocalToDevice", optional_override([](const SkCanvas& self, WASMPointerF32 mPtr) {
            SkScalar* sixteenMatrixValues = reinterpret_cast<SkScalar*>(mPtr);
            if (!sixteenMatrixValues) {
                return; // matrix cannot be null
            }
            SkM44 m = self.getLocalToDevice();
            m.getRowMajor(sixteenMatrixValues);
        }))
        .function("getSaveCount", &SkCanvas::getSaveCount)
        // We allocate room for the matrix from the JS side and free it there so as to not have
        // an awkward moment where we malloc something here and "just know" to free it on the
        // JS side.
        .function("_getTotalMatrix", optional_override([](const SkCanvas& self, WASMPointerU8 mPtr) {
            SkScalar* nineMatrixValues = reinterpret_cast<SkScalar*>(mPtr);
            if (!nineMatrixValues) {
                return; // matrix cannot be null
            }
            SkMatrix m = self.getTotalMatrix();
            m.get9(nineMatrixValues);
        }))
        .function("makeSurface", optional_override([](SkCanvas& self, SimpleImageInfo sii)->sk_sp<SkSurface> {
            return self.makeSurface(toSkImageInfo(sii), nullptr);
        }), allow_raw_pointers())
        .function("markCTM", optional_override([](SkCanvas& self, std::string marker) {
            self.markCTM(marker.c_str());
        }))

        .function("_readPixels", optional_override([](SkCanvas& self, SimpleImageInfo di,
                                                      WASMPointerU8 pPtr,
                                                      size_t dstRowBytes, int srcX, int srcY) {
            uint8_t* pixels = reinterpret_cast<uint8_t*>(pPtr);
            SkImageInfo dstInfo = toSkImageInfo(di);

            return self.readPixels(dstInfo, pixels, dstRowBytes, srcX, srcY);
        }))
        .function("restore", &SkCanvas::restore)
        .function("restoreToCount", &SkCanvas::restoreToCount)
        .function("rotate", select_overload<void (SkScalar, SkScalar, SkScalar)>(&SkCanvas::rotate))
        .function("save", &SkCanvas::save)
        .function("_saveLayer", optional_override([](SkCanvas& self, const SkPaint* p, WASMPointerF32 fPtr,
                                                     const SkImageFilter* backdrop, SkCanvas::SaveLayerFlags flags)->int {
            SkRect* bounds = reinterpret_cast<SkRect*>(fPtr);
            return self.saveLayer(SkCanvas::SaveLayerRec(bounds, p, backdrop, flags));
        }), allow_raw_pointers())
        .function("saveLayerPaint", optional_override([](SkCanvas& self, const SkPaint p)->int {
            return self.saveLayer(SkCanvas::SaveLayerRec(nullptr, &p, 0));
        }))
        .function("scale", &SkCanvas::scale)
        .function("skew", &SkCanvas::skew)
        .function("translate", &SkCanvas::translate)
        .function("_writePixels", optional_override([](SkCanvas& self, SimpleImageInfo di,
                                                       WASMPointerU8 pPtr,
                                                       size_t srcRowBytes, int dstX, int dstY) {
            uint8_t* pixels = reinterpret_cast<uint8_t*>(pPtr);
            SkImageInfo dstInfo = toSkImageInfo(di);

            return self.writePixels(dstInfo, pixels, srcRowBytes, dstX, dstY);
        }));

    class_<SkColorFilter>("ColorFilter")
        .smart_ptr<sk_sp<SkColorFilter>>("sk_sp<ColorFilter>>")
        .class_function("_MakeBlend", optional_override([](WASMPointerF32 cPtr, SkBlendMode mode)->sk_sp<SkColorFilter> {
            return SkColorFilters::Blend(ptrToSkColor4f(cPtr).toSkColor(), mode);
        }))
        .class_function("MakeCompose", &SkColorFilters::Compose)
        .class_function("MakeLerp", &SkColorFilters::Lerp)
        .class_function("MakeLinearToSRGBGamma", &SkColorFilters::LinearToSRGBGamma)
        .class_function("_makeMatrix", optional_override([](WASMPointerF32 fPtr) {
            float* twentyFloats = reinterpret_cast<float*>(fPtr);
            return SkColorFilters::Matrix(twentyFloats);
        }))
        .class_function("MakeSRGBToLinearGamma", &SkColorFilters::SRGBToLinearGamma);

    class_<SkContourMeasureIter>("ContourMeasureIter")
        .constructor<const SkPath&, bool, SkScalar>()
        .function("next", &SkContourMeasureIter::next);

    class_<SkContourMeasure>("ContourMeasure")
        .smart_ptr<sk_sp<SkContourMeasure>>("sk_sp<ContourMeasure>>")
        .function("_getPosTan", optional_override([](SkContourMeasure& self,
                                                     SkScalar distance,
                                                     WASMPointerF32 oPtr) -> void {
            SkPoint* pointAndVector = reinterpret_cast<SkPoint*>(oPtr);
            if (!self.getPosTan(distance, pointAndVector, pointAndVector + 1)) {
                SkDebugf("zero-length path in getPosTan\n");
            }
        }))
        .function("getSegment", optional_override([](SkContourMeasure& self, SkScalar startD,
                                                     SkScalar stopD, bool startWithMoveTo) -> SkPath {
            SkPath p;
            bool ok = self.getSegment(startD, stopD, &p, startWithMoveTo);
            if (ok) {
                return p;
            }
            return SkPath();
        }))
        .function("isClosed", &SkContourMeasure::isClosed)
        .function("length", &SkContourMeasure::length);

#ifndef SK_NO_FONTS
    class_<SkFont>("Font")
        .constructor<>()
        .constructor<sk_sp<SkTypeface>>()
        .constructor<sk_sp<SkTypeface>, SkScalar>()
        .constructor<sk_sp<SkTypeface>, SkScalar, SkScalar, SkScalar>()
        .function("_getGlyphWidthBounds", optional_override([](SkFont& self, WASMPointerU16 gPtr,
                                                          int numGlyphs, WASMPointerF32 wPtr,
                                                          WASMPointerF32 rPtr,
                                                          SkPaint* paint) {
            const SkGlyphID* glyphs = reinterpret_cast<const SkGlyphID*>(gPtr);
            // On the JS side only one of these is set at a time for easier ergonomics.
            SkRect* outputRects = reinterpret_cast<SkRect*>(rPtr);
            SkScalar* outputWidths = reinterpret_cast<SkScalar*>(wPtr);
            self.getWidthsBounds(glyphs, numGlyphs, outputWidths, outputRects, paint);
        }), allow_raw_pointers())
        .function("_getGlyphIDs", optional_override([](SkFont& self, WASMPointerU8 sptr,
                                                       size_t strLen, size_t expectedCodePoints,
                                                       WASMPointerU16 iPtr) -> int {
            char* str = reinterpret_cast<char*>(sptr);
            SkGlyphID* glyphIDs = reinterpret_cast<SkGlyphID*>(iPtr);

            int actualCodePoints = self.textToGlyphs(str, strLen, SkTextEncoding::kUTF8,
                                                     glyphIDs, expectedCodePoints);
            return actualCodePoints;
        }))
        .function("getMetrics", optional_override([](SkFont& self) -> JSObject {
            SkFontMetrics fm;
            self.getMetrics(&fm);

            JSObject j = emscripten::val::object();
            j.set("ascent",  fm.fAscent);
            j.set("descent", fm.fDescent);
            j.set("leading", fm.fLeading);
            if (!(fm.fFlags & SkFontMetrics::kBoundsInvalid_Flag)) {
                const float rect[] = {
                    fm.fXMin, fm.fTop, fm.fXMax, fm.fBottom
                };
                j.set("bounds", MakeTypedArray(4, rect));
            }
            return j;
        }))
        .function("_getGlyphIntercepts", optional_override([](SkFont& self,
                                                             WASMPointerU16 gPtr, size_t numGlyphs, bool ownGlyphs,
                                                             WASMPointerF32 pPtr, size_t numPos, bool ownPos,
                                                             float top, float bottom) -> Float32Array {
            JSSpan<uint16_t> glyphs(gPtr, numGlyphs, ownGlyphs);
            JSSpan<float>    pos   (pPtr, numPos, ownPos);
            if (glyphs.size() > (pos.size() >> 1)) {
                return emscripten::val("Not enough x,y position pairs for glyphs");
            }
            auto sects  = self.getIntercepts(glyphs.data(), SkToInt(glyphs.size()),
                                             (const SkPoint*)pos.data(), top, bottom);
            return MakeTypedArray(sects.size(), (const float*)sects.data());
        }), allow_raw_pointers())
        .function("getScaleX", &SkFont::getScaleX)
        .function("getSize", &SkFont::getSize)
        .function("getSkewX", &SkFont::getSkewX)
        .function("isEmbolden", &SkFont::isEmbolden)
        .function("getTypeface", &SkFont::getTypeface, allow_raw_pointers())
        .function("setEdging", &SkFont::setEdging)
        .function("setEmbeddedBitmaps", &SkFont::setEmbeddedBitmaps)
        .function("setHinting", &SkFont::setHinting)
        .function("setLinearMetrics", &SkFont::setLinearMetrics)
        .function("setScaleX", &SkFont::setScaleX)
        .function("setSize", &SkFont::setSize)
        .function("setSkewX", &SkFont::setSkewX)
        .function("setEmbolden", &SkFont::setEmbolden)
        .function("setSubpixel", &SkFont::setSubpixel)
        .function("setTypeface", &SkFont::setTypeface, allow_raw_pointers());

    class_<SkFontMgr>("FontMgr")
        .smart_ptr<sk_sp<SkFontMgr>>("sk_sp<FontMgr>")
        .class_function("_fromData", optional_override([](WASMPointerU32 dPtr,
                                                          WASMPointerU32 sPtr,
                                                          int numFonts)->sk_sp<SkFontMgr> {
            auto datas = reinterpret_cast<const uint8_t**>(dPtr);
            auto sizes = reinterpret_cast<const size_t*>(sPtr);

            std::unique_ptr<sk_sp<SkData>[]> skdatas(new sk_sp<SkData>[numFonts]);
            for (int i = 0; i < numFonts; ++i) {
                skdatas[i] = SkData::MakeFromMalloc(datas[i], sizes[i]);
            }

            return SkFontMgr_New_Custom_Data(skdatas.get(), numFonts);
        }), allow_raw_pointers())
        .class_function("RefDefault", &SkFontMgr::RefDefault)
        .function("countFamilies", &SkFontMgr::countFamilies)
        .function("getFamilyName", optional_override([](SkFontMgr& self, int index)->JSString {
            if (index < 0 || index >= self.countFamilies()) {
                return emscripten::val::null();
            }
            SkString s;
            self.getFamilyName(index, &s);
            return emscripten::val(s.c_str());
        }))
#ifdef SK_DEBUG
        .function("dumpFamilies", optional_override([](SkFontMgr& self) {
            int numFam = self.countFamilies();
            SkDebugf("There are %d font families\n", numFam);
            for (int i = 0 ; i< numFam; i++) {
                SkString s;
                self.getFamilyName(i, &s);
                SkDebugf("\t%s\n", s.c_str());
            }
        }))
#endif
        .function("_makeTypefaceFromData", optional_override([](SkFontMgr& self,
                                                WASMPointerU8 fPtr,
                                                int flen)->sk_sp<SkTypeface> {
        uint8_t* font = reinterpret_cast<uint8_t*>(fPtr);
        sk_sp<SkData> fontData = SkData::MakeFromMalloc(font, flen);

        return self.makeFromData(fontData);
    }), allow_raw_pointers());
#endif // SK_NO_FONTS

    class_<SkImage>("Image")
        .smart_ptr<sk_sp<SkImage>>("sk_sp<Image>")
        // Note that this needs to be cleaned up with delete().
        .function("getColorSpace", optional_override([](sk_sp<SkImage> self)->sk_sp<SkColorSpace> {
            return self->imageInfo().refColorSpace();
        }), allow_raw_pointers())
        .function("getImageInfo", optional_override([](sk_sp<SkImage> self)->JSObject {
            // We cannot return a SimpleImageInfo because the colorspace object would be leaked.
            JSObject result = emscripten::val::object();
            SkImageInfo ii = self->imageInfo();
            result.set("alphaType", ii.alphaType());
            result.set("colorType", ii.colorType());
            result.set("height", ii.height());
            result.set("width", ii.width());
            return result;
        }))
        .function("height", &SkImage::height)
        .function("encodeToBytes", optional_override([](sk_sp<SkImage> self) -> Uint8Array {
            sk_sp<SkData> data = self->encodeToData();
            if (!data) {
                return emscripten::val::null();
            }
            return toBytes(data);
        }))
       .function("encodeToBytes", optional_override([](sk_sp<SkImage> self,
                                            SkEncodedImageFormat fmt, int quality) -> Uint8Array {
            sk_sp<SkData> data = self->encodeToData(fmt, quality);
            if (!data) {
                return emscripten::val::null();
            }
            return toBytes(data);
        }))
        .function("makeCopyWithDefaultMipmaps", optional_override([](sk_sp<SkImage> self)->sk_sp<SkImage> {
            return self->withDefaultMipmaps();
        }))
        .function("_makeShaderCubic", optional_override([](sk_sp<SkImage> self,
                                 SkTileMode tx, SkTileMode ty,
                                 float B, float C, // See SkSamplingOptions.h for docs.
                                 WASMPointerF32 mPtr)->sk_sp<SkShader> {
            return self->makeShader(tx, ty, SkSamplingOptions({B, C}), OptionalMatrix(mPtr));
        }), allow_raw_pointers())
        .function("_makeShaderOptions", optional_override([](sk_sp<SkImage> self,
                                 SkTileMode tx, SkTileMode ty,
                                 SkFilterMode filter, SkMipmapMode mipmap,
                                 WASMPointerF32 mPtr)->sk_sp<SkShader> {
            return self->makeShader(tx, ty, {filter, mipmap}, OptionalMatrix(mPtr));
        }), allow_raw_pointers())
        .function("_readPixels", optional_override([](sk_sp<SkImage> self,
                                 SimpleImageInfo sii, WASMPointerU8 pPtr,
                                 size_t dstRowBytes, int srcX, int srcY)->bool {
            uint8_t* pixels = reinterpret_cast<uint8_t*>(pPtr);
            SkImageInfo ii = toSkImageInfo(sii);
            // TODO(adlai) Migrate CanvasKit API to require DirectContext arg here.
            GrDirectContext* dContext = nullptr;
#ifdef SK_GL
            dContext = GrAsDirectContext(as_IB(self.get())->context());
#endif
            return self->readPixels(dContext, ii, pixels, dstRowBytes, srcX, srcY);
        }), allow_raw_pointers())
        .function("width", &SkImage::width);

    class_<SkImageFilter>("ImageFilter")
        .smart_ptr<sk_sp<SkImageFilter>>("sk_sp<ImageFilter>")
        .class_function("MakeBlur", optional_override([](SkScalar sigmaX, SkScalar sigmaY,
                                                         SkTileMode tileMode, sk_sp<SkImageFilter> input)->sk_sp<SkImageFilter> {
            return SkImageFilters::Blur(sigmaX, sigmaY, tileMode, input);
        }))
        .class_function("MakeColorFilter", optional_override([](sk_sp<SkColorFilter> cf,
                                                                sk_sp<SkImageFilter> input)->sk_sp<SkImageFilter> {
            return SkImageFilters::ColorFilter(cf, input);
        }))
        .class_function("MakeCompose", &SkImageFilters::Compose)
        .class_function("_MakeMatrixTransformCubic",
                        optional_override([](WASMPointerF32 mPtr, float B, float C,
                                             sk_sp<SkImageFilter> input)->sk_sp<SkImageFilter> {
            OptionalMatrix matr(mPtr);
            return SkImageFilters::MatrixTransform(matr, SkSamplingOptions({B, C}), input);
        }))
        .class_function("_MakeMatrixTransformOptions",
                        optional_override([](WASMPointerF32 mPtr, SkFilterMode fm, SkMipmapMode mm,
                                             sk_sp<SkImageFilter> input)->sk_sp<SkImageFilter> {
            OptionalMatrix matr(mPtr);
            return SkImageFilters::MatrixTransform(matr, SkSamplingOptions(fm, mm), input);
        }));

    class_<SkMaskFilter>("MaskFilter")
        .smart_ptr<sk_sp<SkMaskFilter>>("sk_sp<MaskFilter>")
        .class_function("MakeBlur", optional_override([](SkBlurStyle style, SkScalar sigma, bool respectCTM)->sk_sp<SkMaskFilter> {
        // Adds a little helper because emscripten doesn't expose default params.
        return SkMaskFilter::MakeBlur(style, sigma, respectCTM);
    }), allow_raw_pointers());

    class_<SkPaint>("Paint")
        .constructor<>()
        .function("copy", optional_override([](const SkPaint& self)->SkPaint {
            SkPaint p(self);
            return p;
        }))
        // provide an allocated place to put the returned color
        .function("_getColor", optional_override([](SkPaint& self, WASMPointerF32 cPtr)->void {
            const SkColor4f& c = self.getColor4f();
            float* fourFloats = reinterpret_cast<float*>(cPtr);
            memcpy(fourFloats, c.vec(), 4 * sizeof(SkScalar));
        }))
        .function("getStrokeCap", &SkPaint::getStrokeCap)
        .function("getStrokeJoin", &SkPaint::getStrokeJoin)
        .function("getStrokeMiter", &SkPaint::getStrokeMiter)
        .function("getStrokeWidth", &SkPaint::getStrokeWidth)
        .function("setAntiAlias", &SkPaint::setAntiAlias)
        .function("setAlphaf", &SkPaint::setAlphaf)
        .function("setBlendMode", &SkPaint::setBlendMode)
        .function("_setColor", optional_override([](SkPaint& self, WASMPointerF32 cPtr,
                sk_sp<SkColorSpace> colorSpace) {
            self.setColor(ptrToSkColor4f(cPtr), colorSpace.get());
        }))
        .function("setColorInt", optional_override([](SkPaint& self, SkColor color) {
            self.setColor(SkColor4f::FromColor(color), nullptr);
        }))
        .function("setColorInt", optional_override([](SkPaint& self, SkColor color,
                sk_sp<SkColorSpace> colorSpace) {
            self.setColor(SkColor4f::FromColor(color), colorSpace.get());
        }))
        .function("setColorFilter", &SkPaint::setColorFilter)
        .function("setImageFilter", &SkPaint::setImageFilter)
        .function("setMaskFilter", &SkPaint::setMaskFilter)
        .function("setPathEffect", &SkPaint::setPathEffect)
        .function("setShader", &SkPaint::setShader)
        .function("setStrokeCap", &SkPaint::setStrokeCap)
        .function("setStrokeJoin", &SkPaint::setStrokeJoin)
        .function("setStrokeMiter", &SkPaint::setStrokeMiter)
        .function("setStrokeWidth", &SkPaint::setStrokeWidth)
        .function("setStyle", &SkPaint::setStyle);

    class_<SkColorSpace>("ColorSpace")
        .smart_ptr<sk_sp<SkColorSpace>>("sk_sp<ColorSpace>")
        .class_function("Equals", optional_override([](sk_sp<SkColorSpace> a, sk_sp<SkColorSpace> b)->bool {
            return SkColorSpace::Equals(a.get(), b.get());
        }))
        // These are private because they are to be called once in interface.js to
        // avoid clients having to delete the returned objects.
        .class_function("_MakeSRGB", &SkColorSpace::MakeSRGB)
        .class_function("_MakeDisplayP3", optional_override([]()->sk_sp<SkColorSpace> {
            return SkColorSpace::MakeRGB(SkNamedTransferFn::kSRGB, SkNamedGamut::kDisplayP3);
        }))
        .class_function("_MakeAdobeRGB", optional_override([]()->sk_sp<SkColorSpace> {
            return SkColorSpace::MakeRGB(SkNamedTransferFn::k2Dot2, SkNamedGamut::kAdobeRGB);
        }));

    class_<SkPathEffect>("PathEffect")
        .smart_ptr<sk_sp<SkPathEffect>>("sk_sp<PathEffect>")
        .class_function("MakeCorner", &SkCornerPathEffect::Make)
        .class_function("_MakeDash", optional_override([](WASMPointerF32 cptr, int count,
                                                          SkScalar phase)->sk_sp<SkPathEffect> {
            const float* intervals = reinterpret_cast<const float*>(cptr);
            return SkDashPathEffect::Make(intervals, count, phase);
        }), allow_raw_pointers())
        .class_function("MakeDiscrete", &SkDiscretePathEffect::Make);

    // TODO(kjlubick, reed) Make SkPath immutable and only creatable via a factory/builder.
    class_<SkPath>("Path")
        .constructor<>()
#ifdef SK_INCLUDE_PATHOPS
        .class_function("MakeFromOp", &MakePathFromOp)
#endif
        .class_function("MakeFromSVGString", &MakePathFromSVGString)
        .class_function("_MakeFromCmds", &MakePathFromCmds)
        .class_function("_MakeFromVerbsPointsWeights", &MakePathFromVerbsPointsWeights)
        .function("_addArc", optional_override([](SkPath& self,
                                                   WASMPointerF32 fPtr,
                                                   SkScalar startAngle, SkScalar sweepAngle)->void {
            const SkRect* oval = reinterpret_cast<const SkRect*>(fPtr);
            self.addArc(*oval, startAngle, sweepAngle);
        }))
        .function("_addOval", optional_override([](SkPath& self,
                                                   WASMPointerF32 fPtr,
                                                   bool ccw, unsigned start)->void {
            const SkRect* oval = reinterpret_cast<const SkRect*>(fPtr);
            self.addOval(*oval, ccw ? SkPathDirection::kCCW : SkPathDirection::kCW, start);
        }))
        // interface.js has 3 overloads of addPath
        .function("_addPath", &ApplyAddPath)
        .function("_addPoly", optional_override([](SkPath& self,
                                                   WASMPointerF32 fPtr,
                                                   int count, bool close)->void {
            const SkPoint* pts = reinterpret_cast<const SkPoint*>(fPtr);
            self.addPoly(pts, count, close);
        }))
        .function("_addRect", optional_override([](SkPath& self,
                                                   WASMPointerF32 fPtr,
                                                   bool ccw)->void {
            const SkRect* rect = reinterpret_cast<const SkRect*>(fPtr);
            self.addRect(*rect, ccw ? SkPathDirection::kCCW : SkPathDirection::kCW);
        }))
        .function("_addRRect", optional_override([](SkPath& self,
                                                   WASMPointerF32 fPtr,
                                                   bool ccw)->void {
            self.addRRect(ptrToSkRRect(fPtr), ccw ? SkPathDirection::kCCW : SkPathDirection::kCW);
        }))
        .function("_addVerbsPointsWeights", &PathAddVerbsPointsWeights)
        .function("_arcToOval", optional_override([](SkPath& self,
                                                   WASMPointerF32 fPtr, SkScalar startAngle,
                                                   SkScalar sweepAngle, bool forceMoveTo)->void {
            const SkRect* oval = reinterpret_cast<const SkRect*>(fPtr);
            self.arcTo(*oval, startAngle, sweepAngle, forceMoveTo);
        }))
        .function("_arcToRotated", &ApplyArcToArcSize)
        .function("_arcToTangent", ApplyArcToTangent)
        .function("_close", &ApplyClose)
        .function("_conicTo", &ApplyConicTo)
        .function("countPoints", &SkPath::countPoints)
        .function("contains", &SkPath::contains)
        .function("_cubicTo", &ApplyCubicTo)
        .function("_getPoint", optional_override([](SkPath& self, int index,
                                                    WASMPointerF32 oPtr)->void {
            SkPoint* output = reinterpret_cast<SkPoint*>(oPtr);
            *output = self.getPoint(index);
        }))
        .function("isEmpty",  &SkPath::isEmpty)
        .function("isVolatile", &SkPath::isVolatile)
        .function("_lineTo", &ApplyLineTo)
        .function("_moveTo", &ApplyMoveTo)
        .function("_quadTo", &ApplyQuadTo)
        .function("_rArcTo", &ApplyRArcToArcSize)
        .function("_rConicTo", &ApplyRConicTo)
        .function("_rCubicTo", &ApplyRCubicTo)
        .function("_rLineTo", &ApplyRLineTo)
        .function("_rMoveTo", &ApplyRMoveTo)
        .function("_rQuadTo", &ApplyRQuadTo)
        .function("reset", &ApplyReset)
        .function("rewind", &ApplyRewind)
        .function("setIsVolatile", &SkPath::setIsVolatile)
        .function("_transform", select_overload<void(SkPath&, SkScalar, SkScalar, SkScalar, SkScalar, SkScalar, SkScalar, SkScalar, SkScalar, SkScalar)>(&ApplyTransform))

        // PathEffects
        .function("_dash", &ApplyDash)
        .function("_trim", &ApplyTrim)
        .function("_stroke", &ApplyStroke)

#ifdef SK_INCLUDE_PATHOPS
        // PathOps
        .function("_simplify", &ApplySimplify)
        .function("_op", &ApplyPathOp)
        .function("makeAsWinding", &MakeAsWinding)
#endif
        // Exporting
        .function("toSVGString", &ToSVGString)
        .function("toCmds", &ToCmds)

        .function("setFillType", select_overload<void(SkPathFillType)>(&SkPath::setFillType))
        .function("getFillType", &SkPath::getFillType)
        .function("_getBounds", optional_override([](SkPath& self,
                                                     WASMPointerF32 fPtr)->void {
            SkRect* output = reinterpret_cast<SkRect*>(fPtr);
            output[0] = self.getBounds();
        }))
        .function("_computeTightBounds", optional_override([](SkPath& self,
                                                              WASMPointerF32 fPtr)->void {
            SkRect* output = reinterpret_cast<SkRect*>(fPtr);
            output[0] = self.computeTightBounds();
        }))
        .function("equals", &Equals)
        .function("copy", &CopyPath)
#ifdef SK_DEBUG
        .function("dump", select_overload<void() const>(&SkPath::dump))
        .function("dumpHex", select_overload<void() const>(&SkPath::dumpHex))
#endif
        ;

    class_<SkPictureRecorder>("PictureRecorder")
        .constructor<>()
        .function("_beginRecording", optional_override([](SkPictureRecorder& self,
                                                          WASMPointerF32 fPtr) -> SkCanvas* {
            SkRect* bounds = reinterpret_cast<SkRect*>(fPtr);
            return self.beginRecording(*bounds, nullptr);
        }), allow_raw_pointers())
        .function("finishRecordingAsPicture", optional_override([](SkPictureRecorder& self)
                                                                   -> sk_sp<SkPicture> {
            return self.finishRecordingAsPicture();
        }), allow_raw_pointers());

    class_<SkPicture>("Picture")
        .smart_ptr<sk_sp<SkPicture>>("sk_sp<Picture>")
#ifdef SK_SERIALIZE_SKP
        // The serialized format of an SkPicture (informally called an "skp"), is not something
        // that clients should ever rely on.  The format may change at anytime and no promises
        // are made for backwards or forward compatibility.
        .function("serialize", optional_override([](SkPicture& self) -> Uint8Array {
            // Emscripten doesn't play well with optional arguments, which we don't
            // want to expose anyway.
            sk_sp<SkData> data = self.serialize();
            if (!data) {
                return emscripten::val::null();
            }
            return toBytes(data);
        }), allow_raw_pointers())
#endif
    ;

    class_<SkShader>("Shader")
        .smart_ptr<sk_sp<SkShader>>("sk_sp<Shader>")
        .class_function("MakeBlend", select_overload<sk_sp<SkShader>(SkBlendMode, sk_sp<SkShader>, sk_sp<SkShader>)>(&SkShaders::Blend))
        .class_function("_MakeColor",
            optional_override([](WASMPointerF32 cPtr, sk_sp<SkColorSpace> colorSpace)->sk_sp<SkShader> {
                return SkShaders::Color(ptrToSkColor4f(cPtr), colorSpace);
            })
        )
        .class_function("MakeFractalNoise", optional_override([](
                                                SkScalar baseFreqX, SkScalar baseFreqY,
                                                int numOctaves, SkScalar seed,
                                                int tileW, int tileH)->sk_sp<SkShader> {
            // if tileSize is empty (e.g. tileW <= 0 or tileH <= 0, it will be ignored.
            SkISize tileSize = SkISize::Make(tileW, tileH);
            return SkPerlinNoiseShader::MakeFractalNoise(baseFreqX, baseFreqY,
                                                         numOctaves, seed, &tileSize);
        }))
         // Here and in other gradient functions, cPtr is a pointer to an array of data
         // representing colors. whether this is an array of SkColor or SkColor4f is indicated
         // by the colorType argument. Only RGBA_8888 and RGBA_F32 are accepted.
        .class_function("_MakeLinearGradient", optional_override([](
                                         WASMPointerF32 fourFloatsPtr,
                                         WASMPointerF32 cPtr, SkColorType colorType,
                                         WASMPointerF32 pPtr,
                                         int count, SkTileMode mode, uint32_t flags,
                                         WASMPointerF32 mPtr,
                                         sk_sp<SkColorSpace> colorSpace)->sk_sp<SkShader> {
             const SkPoint* points = reinterpret_cast<const SkPoint*>(fourFloatsPtr);
             const SkScalar* positions = reinterpret_cast<const SkScalar*>(pPtr);
             OptionalMatrix localMatrix(mPtr);

             if (colorType == SkColorType::kRGBA_F32_SkColorType) {
                 const SkColor4f* colors  = reinterpret_cast<const SkColor4f*>(cPtr);
                 return SkGradientShader::MakeLinear(points, colors, colorSpace, positions, count,
                                                     mode, flags, &localMatrix);
             } else if (colorType == SkColorType::kRGBA_8888_SkColorType) {
                 const SkColor* colors  = reinterpret_cast<const SkColor*>(cPtr);
                 return SkGradientShader::MakeLinear(points, colors, positions, count,
                                                     mode, flags, &localMatrix);
             }
             SkDebugf("%d is not an accepted colorType\n", colorType);
             return nullptr;
         }), allow_raw_pointers())
        .class_function("_MakeRadialGradient", optional_override([](
                                         SkScalar cx, SkScalar cy, SkScalar radius,
                                         WASMPointerF32 cPtr, SkColorType colorType,
                                         WASMPointerF32 pPtr,
                                         int count, SkTileMode mode, uint32_t flags,
                                         WASMPointerF32 mPtr,
                                         sk_sp<SkColorSpace> colorSpace)->sk_sp<SkShader> {
            const SkScalar* positions = reinterpret_cast<const SkScalar*>(pPtr);
            OptionalMatrix localMatrix(mPtr);
            if (colorType == SkColorType::kRGBA_F32_SkColorType) {
               const SkColor4f* colors  = reinterpret_cast<const SkColor4f*>(cPtr);
               return SkGradientShader::MakeRadial({cx, cy}, radius, colors, colorSpace,
                                                   positions, count, mode, flags, &localMatrix);
            } else if (colorType == SkColorType::kRGBA_8888_SkColorType) {
               const SkColor* colors  = reinterpret_cast<const SkColor*>(cPtr);
               return SkGradientShader::MakeRadial({cx, cy}, radius, colors, positions,
                                                   count, mode, flags, &localMatrix);
            }
            SkDebugf("%d is not an accepted colorType\n", colorType);
            return nullptr;
        }), allow_raw_pointers())
        .class_function("_MakeSweepGradient", optional_override([](SkScalar cx, SkScalar cy,
                                         WASMPointerF32 cPtr, SkColorType colorType,
                                         WASMPointerF32 pPtr,
                                         int count, SkTileMode mode,
                                         SkScalar startAngle, SkScalar endAngle,
                                         uint32_t flags,
                                         WASMPointerF32 mPtr,
                                         sk_sp<SkColorSpace> colorSpace)->sk_sp<SkShader> {
            const SkScalar* positions = reinterpret_cast<const SkScalar*>(pPtr);
            OptionalMatrix localMatrix(mPtr);
            if (colorType == SkColorType::kRGBA_F32_SkColorType) {
               const SkColor4f* colors  = reinterpret_cast<const SkColor4f*>(cPtr);
               return SkGradientShader::MakeSweep(cx, cy, colors, colorSpace, positions, count,
                                                  mode, startAngle, endAngle, flags,
                                                  &localMatrix);
            } else if (colorType == SkColorType::kRGBA_8888_SkColorType) {
               const SkColor* colors  = reinterpret_cast<const SkColor*>(cPtr);
               return SkGradientShader::MakeSweep(cx, cy, colors, positions, count,
                                                  mode, startAngle, endAngle, flags,
                                                  &localMatrix);
            }
            SkDebugf("%d is not an accepted colorType\n", colorType);
            return nullptr;
        }), allow_raw_pointers())
        .class_function("MakeTurbulence", optional_override([](
                                                SkScalar baseFreqX, SkScalar baseFreqY,
                                                int numOctaves, SkScalar seed,
                                                int tileW, int tileH)->sk_sp<SkShader> {
            // if tileSize is empty (e.g. tileW <= 0 or tileH <= 0, it will be ignored.
            SkISize tileSize = SkISize::Make(tileW, tileH);
            return SkPerlinNoiseShader::MakeTurbulence(baseFreqX, baseFreqY,
                                                       numOctaves, seed, &tileSize);
        }))
        .class_function("_MakeTwoPointConicalGradient", optional_override([](
                                         WASMPointerF32 fourFloatsPtr,
                                         SkScalar startRadius, SkScalar endRadius,
                                         WASMPointerF32 cPtr, SkColorType colorType,
                                         WASMPointerF32 pPtr,
                                         int count, SkTileMode mode, uint32_t flags,
                                         WASMPointerF32 mPtr,
                                         sk_sp<SkColorSpace> colorSpace)->sk_sp<SkShader> {
            const SkPoint* startAndEnd = reinterpret_cast<const SkPoint*>(fourFloatsPtr);
            const SkScalar* positions = reinterpret_cast<const SkScalar*>(pPtr);
            OptionalMatrix localMatrix(mPtr);

            if (colorType == SkColorType::kRGBA_F32_SkColorType) {
               const SkColor4f* colors  = reinterpret_cast<const SkColor4f*>(cPtr);
               return SkGradientShader::MakeTwoPointConical(startAndEnd[0], startRadius,
                                                            startAndEnd[1], endRadius,
                                                            colors, colorSpace, positions, count, mode,
                                                            flags, &localMatrix);
            } else if (colorType == SkColorType::kRGBA_8888_SkColorType) {
               const SkColor* colors  = reinterpret_cast<const SkColor*>(cPtr);
               return SkGradientShader::MakeTwoPointConical(startAndEnd[0], startRadius,
                                                            startAndEnd[1], endRadius,
                                                            colors, positions, count, mode,
                                                            flags, &localMatrix);
            }
            SkDebugf("%d is not an accepted colorType\n", colorType);
            return nullptr;
        }), allow_raw_pointers());

#ifdef SK_INCLUDE_RUNTIME_EFFECT
    class_<SkRuntimeEffect>("RuntimeEffect")
        .smart_ptr<sk_sp<SkRuntimeEffect>>("sk_sp<RuntimeEffect>")
        .class_function("_Make", optional_override([](std::string sksl,
                                                     emscripten::val errHandler
                                                    )->sk_sp<SkRuntimeEffect> {
            SkString s(sksl.c_str(), sksl.length());
            auto [effect, errorText] = SkRuntimeEffect::MakeForShader(s);
            if (!effect) {
                errHandler.call<void>("onError", val(errorText.c_str()));
                return nullptr;
            }
            return effect;
        }))
        .function("_makeShader", optional_override([](SkRuntimeEffect& self, WASMPointerF32 fPtr, size_t fLen, bool isOpaque,
                                                      WASMPointerF32 mPtr)->sk_sp<SkShader> {
            void* inputData = reinterpret_cast<void*>(fPtr);
            castUniforms(inputData, fLen, self);
            sk_sp<SkData> inputs = SkData::MakeFromMalloc(inputData, fLen);

            OptionalMatrix localMatrix(mPtr);
            return self.makeShader(inputs, nullptr, 0, &localMatrix, isOpaque);
        }))
        .function("_makeShaderWithChildren", optional_override([](SkRuntimeEffect& self, WASMPointerF32 fPtr, size_t fLen, bool isOpaque,
                                                                  WASMPointerU32 cPtrs, size_t cLen,
                                                                  WASMPointerF32 mPtr)->sk_sp<SkShader> {
            void* inputData = reinterpret_cast<void*>(fPtr);
            castUniforms(inputData, fLen, self);
            sk_sp<SkData> inputs = SkData::MakeFromMalloc(inputData, fLen);

            sk_sp<SkShader>* children = new sk_sp<SkShader>[cLen];
            SkShader** childrenPtrs = reinterpret_cast<SkShader**>(cPtrs);
            for (size_t i = 0; i < cLen; i++) {
                // This bare pointer was already part of an sk_sp (owned outside of here),
                // so we want to ref the new sk_sp so makeShader doesn't clean it up.
                children[i] = sk_ref_sp<SkShader>(childrenPtrs[i]);
            }
            OptionalMatrix localMatrix(mPtr);
            auto s = self.makeShader(inputs, children, cLen, &localMatrix, isOpaque);
            delete[] children;
            return s;
        }))
        .function("getUniformCount", optional_override([](SkRuntimeEffect& self)->int {
            return self.uniforms().count();
        }))
        .function("getUniformFloatCount", optional_override([](SkRuntimeEffect& self)->int {
            return self.uniformSize() / sizeof(float);
        }))
        .function("getUniformName", optional_override([](SkRuntimeEffect& self, int i)->JSString {
            auto it = self.uniforms().begin() + i;
            return emscripten::val(it->name.c_str());
        }))
        .function("getUniform", optional_override([](SkRuntimeEffect& self, int i)->RuntimeEffectUniform {
            auto it = self.uniforms().begin() + i;
            RuntimeEffectUniform su = fromUniform(*it);
            return su;
        }));

    value_object<RuntimeEffectUniform>("RuntimeEffectUniform")
        .field("columns",   &RuntimeEffectUniform::columns)
        .field("rows",      &RuntimeEffectUniform::rows)
        .field("slot",      &RuntimeEffectUniform::slot)
        .field("isInteger", &RuntimeEffectUniform::isInteger);

    constant("rt_effect", true);
#endif

    class_<SkSurface>("Surface")
        .smart_ptr<sk_sp<SkSurface>>("sk_sp<Surface>")
        .class_function("_makeRasterDirect", optional_override([](const SimpleImageInfo ii,
                                                                  WASMPointerU8 pPtr,
                                                                  size_t rowBytes)->sk_sp<SkSurface> {
            uint8_t* pixels = reinterpret_cast<uint8_t*>(pPtr);
            SkImageInfo imageInfo = toSkImageInfo(ii);
            return SkSurface::MakeRasterDirect(imageInfo, pixels, rowBytes, nullptr);
        }), allow_raw_pointers())
        .function("_flush", optional_override([](SkSurface& self) {
            self.flushAndSubmit(false);
        }))
        .function("getCanvas", &SkSurface::getCanvas, allow_raw_pointers())
        .function("imageInfo", optional_override([](SkSurface& self)->SimpleImageInfo {
            const auto& ii = self.imageInfo();
            return {ii.width(), ii.height(), ii.colorType(), ii.alphaType(), ii.refColorSpace()};
        }))
        .function("height", &SkSurface::height)
#ifdef SK_GL
        .function("_makeImageFromTexture", optional_override([](SkSurface& self,
                                                uint32_t webglHandle, uint32_t texHandle,
                                                SimpleImageInfo ii)->sk_sp<SkImage> {
            auto releaseCtx = new TextureReleaseContext{webglHandle, texHandle};
            GrGLTextureInfo gti = {GR_GL_TEXTURE_2D, texHandle,
                                   GR_GL_RGBA8}; // TODO(kjlubick) look at ii for this
            GrBackendTexture gbt(ii.width, ii.height, GrMipmapped::kNo, gti);
            auto dContext = GrAsDirectContext(self.getCanvas()->recordingContext());

            return SkImage::MakeFromTexture(
                             dContext,
                             gbt,
                             GrSurfaceOrigin::kTopLeft_GrSurfaceOrigin,
                             ii.colorType,
                             ii.alphaType,
                             ii.colorSpace,
                             deleteJSTexture,
                             releaseCtx);
         }))
 #endif
        .function("_makeImageSnapshot",  optional_override([](SkSurface& self, WASMPointerU32 iPtr)->sk_sp<SkImage> {
            SkIRect* bounds = reinterpret_cast<SkIRect*>(iPtr);
            if (!bounds) {
                return self.makeImageSnapshot();
            }
            return self.makeImageSnapshot(*bounds);
        }))
        .function("makeSurface", optional_override([](SkSurface& self, SimpleImageInfo sii)->sk_sp<SkSurface> {
            return self.makeSurface(toSkImageInfo(sii));
        }), allow_raw_pointers())
#ifdef SK_GL
        .function("reportBackendTypeIsGPU", optional_override([](SkSurface& self) -> bool {
            return self.getCanvas()->recordingContext() != nullptr;
        }))
        .function("sampleCnt", optional_override([](SkSurface& self)->int {
            auto backendRT = self.getBackendRenderTarget(SkSurface::kFlushRead_BackendHandleAccess);
            return (backendRT.isValid()) ? backendRT.sampleCnt() : 0;
        }))
#else
        .function("reportBackendTypeIsGPU", optional_override([](SkSurface& self) -> bool {
            return false;
        }))
#endif
        .function("width", &SkSurface::width);

#ifndef SK_NO_FONTS
    class_<SkTextBlob>("TextBlob")
        .smart_ptr<sk_sp<SkTextBlob>>("sk_sp<TextBlob>")
        .class_function("_MakeFromRSXform", optional_override([](WASMPointerU8 sptr,
                                                              size_t strBtyes,
                                                              WASMPointerF32 xptr,
                                                              const SkFont& font)->sk_sp<SkTextBlob> {
            const char* str = reinterpret_cast<const char*>(sptr);
            const SkRSXform* xforms = reinterpret_cast<const SkRSXform*>(xptr);

            return SkTextBlob::MakeFromRSXform(str, strBtyes, xforms, font, SkTextEncoding::kUTF8);
        }), allow_raw_pointers())
        .class_function("_MakeFromRSXformGlyphs", optional_override([](WASMPointerU16 gPtr,
                                                              size_t byteLen,
                                                              WASMPointerF32 xptr,
                                                              const SkFont& font)->sk_sp<SkTextBlob> {
            const SkGlyphID* glyphs = reinterpret_cast<const SkGlyphID*>(gPtr);
            const SkRSXform* xforms = reinterpret_cast<const SkRSXform*>(xptr);

            return SkTextBlob::MakeFromRSXform(glyphs, byteLen, xforms, font, SkTextEncoding::kGlyphID);
        }), allow_raw_pointers())
        .class_function("_MakeFromText", optional_override([](WASMPointerU8 sptr,
                                                              size_t len, const SkFont& font)->sk_sp<SkTextBlob> {
            const char* str = reinterpret_cast<const char*>(sptr);
            return SkTextBlob::MakeFromText(str, len, font, SkTextEncoding::kUTF8);
        }), allow_raw_pointers())
        .class_function("_MakeFromGlyphs", optional_override([](WASMPointerU16 gPtr,
                                                                size_t byteLen, const SkFont& font)->sk_sp<SkTextBlob> {
            const SkGlyphID* glyphs = reinterpret_cast<const SkGlyphID*>(gPtr);
            return SkTextBlob::MakeFromText(glyphs, byteLen, font, SkTextEncoding::kGlyphID);
        }), allow_raw_pointers());

    class_<SkTypeface>("Typeface")
        .smart_ptr<sk_sp<SkTypeface>>("sk_sp<Typeface>")
        .class_function("_MakeFreeTypeFaceFromData", optional_override([](WASMPointerU8 fPtr,
                                                int flen)->sk_sp<SkTypeface> {
            uint8_t* font = reinterpret_cast<uint8_t*>(fPtr);
            sk_sp<SkData> fontData = SkData::MakeFromMalloc(font, flen);

            return SkFontMgr::RefDefault()->makeFromData(fontData);
        }), allow_raw_pointers())
        .function("_getGlyphIDs", optional_override([](SkTypeface& self, WASMPointerU8 sptr,
                                                   size_t strLen, size_t expectedCodePoints,
                                                   WASMPointerU16 iPtr) -> int {
            char* str = reinterpret_cast<char*>(sptr);
            SkGlyphID* glyphIDs = reinterpret_cast<SkGlyphID*>(iPtr);

            int actualCodePoints = self.textToGlyphs(str, strLen, SkTextEncoding::kUTF8,
                                                     glyphIDs, expectedCodePoints);
            return actualCodePoints;
        }));
#endif

    class_<SkVertices>("Vertices")
        .smart_ptr<sk_sp<SkVertices>>("sk_sp<Vertices>")
        .function("_bounds", optional_override([](SkVertices& self,
                                                  WASMPointerF32 fPtr)->void {
            SkRect* output = reinterpret_cast<SkRect*>(fPtr);
            output[0] = self.bounds();
        }))
        .function("uniqueID", &SkVertices::uniqueID);

    // Not intended to be called directly by clients
    class_<SkVertices::Builder>("_VerticesBuilder")
        .constructor<SkVertices::VertexMode, int, int, uint32_t>()
        .function("colors", optional_override([](SkVertices::Builder& self)->WASMPointerF32{
            // Emscripten won't let us return bare pointers, but we can return ints just fine.
            return reinterpret_cast<WASMPointerF32>(self.colors());
        }))
        .function("detach", &SkVertices::Builder::detach)
        .function("indices", optional_override([](SkVertices::Builder& self)->WASMPointerU16{
            // Emscripten won't let us return bare pointers, but we can return ints just fine.
            return reinterpret_cast<WASMPointerU16>(self.indices());
        }))
        .function("positions", optional_override([](SkVertices::Builder& self)->WASMPointerF32{
            // Emscripten won't let us return bare pointers, but we can return ints just fine.
            return reinterpret_cast<WASMPointerF32>(self.positions());
        }))
        .function("texCoords", optional_override([](SkVertices::Builder& self)->WASMPointerF32{
            // Emscripten won't let us return bare pointers, but we can return ints just fine.
            return reinterpret_cast<WASMPointerF32>(self.texCoords());
        }));

    enum_<SkAlphaType>("AlphaType")
        .value("Opaque",   SkAlphaType::kOpaque_SkAlphaType)
        .value("Premul",   SkAlphaType::kPremul_SkAlphaType)
        .value("Unpremul", SkAlphaType::kUnpremul_SkAlphaType);

    enum_<SkBlendMode>("BlendMode")
        .value("Clear",      SkBlendMode::kClear)
        .value("Src",        SkBlendMode::kSrc)
        .value("Dst",        SkBlendMode::kDst)
        .value("SrcOver",    SkBlendMode::kSrcOver)
        .value("DstOver",    SkBlendMode::kDstOver)
        .value("SrcIn",      SkBlendMode::kSrcIn)
        .value("DstIn",      SkBlendMode::kDstIn)
        .value("SrcOut",     SkBlendMode::kSrcOut)
        .value("DstOut",     SkBlendMode::kDstOut)
        .value("SrcATop",    SkBlendMode::kSrcATop)
        .value("DstATop",    SkBlendMode::kDstATop)
        .value("Xor",        SkBlendMode::kXor)
        .value("Plus",       SkBlendMode::kPlus)
        .value("Modulate",   SkBlendMode::kModulate)
        .value("Screen",     SkBlendMode::kScreen)
        .value("Overlay",    SkBlendMode::kOverlay)
        .value("Darken",     SkBlendMode::kDarken)
        .value("Lighten",    SkBlendMode::kLighten)
        .value("ColorDodge", SkBlendMode::kColorDodge)
        .value("ColorBurn",  SkBlendMode::kColorBurn)
        .value("HardLight",  SkBlendMode::kHardLight)
        .value("SoftLight",  SkBlendMode::kSoftLight)
        .value("Difference", SkBlendMode::kDifference)
        .value("Exclusion",  SkBlendMode::kExclusion)
        .value("Multiply",   SkBlendMode::kMultiply)
        .value("Hue",        SkBlendMode::kHue)
        .value("Saturation", SkBlendMode::kSaturation)
        .value("Color",      SkBlendMode::kColor)
        .value("Luminosity", SkBlendMode::kLuminosity);

    enum_<SkBlurStyle>("BlurStyle")
        .value("Normal", SkBlurStyle::kNormal_SkBlurStyle)
        .value("Solid",  SkBlurStyle::kSolid_SkBlurStyle)
        .value("Outer",  SkBlurStyle::kOuter_SkBlurStyle)
        .value("Inner",  SkBlurStyle::kInner_SkBlurStyle);

    enum_<SkClipOp>("ClipOp")
        .value("Difference", SkClipOp::kDifference)
        .value("Intersect",  SkClipOp::kIntersect);

    enum_<SkColorType>("ColorType")
        .value("Alpha_8", SkColorType::kAlpha_8_SkColorType)
        .value("RGB_565", SkColorType::kRGB_565_SkColorType)
        .value("RGBA_8888", SkColorType::kRGBA_8888_SkColorType)
        .value("BGRA_8888", SkColorType::kBGRA_8888_SkColorType)
        .value("RGBA_1010102", SkColorType::kRGBA_1010102_SkColorType)
        .value("RGB_101010x", SkColorType::kRGB_101010x_SkColorType)
        .value("Gray_8", SkColorType::kGray_8_SkColorType)
        .value("RGBA_F16", SkColorType::kRGBA_F16_SkColorType)
        .value("RGBA_F32", SkColorType::kRGBA_F32_SkColorType);

    enum_<SkPathFillType>("FillType")
        .value("Winding",           SkPathFillType::kWinding)
        .value("EvenOdd",           SkPathFillType::kEvenOdd);

    enum_<SkFilterMode>("FilterMode")
        .value("Nearest",   SkFilterMode::kNearest)
        .value("Linear",    SkFilterMode::kLinear);

    // Only used to control the encode function.
    // TODO(kjlubick): compile these out when the appropriate encoder is disabled.
    enum_<SkEncodedImageFormat>("ImageFormat")
        .value("PNG",  SkEncodedImageFormat::kPNG)
        .value("JPEG",  SkEncodedImageFormat::kJPEG)
        .value("WEBP",  SkEncodedImageFormat::kWEBP);

    enum_<SkMipmapMode>("MipmapMode")
        .value("None",    SkMipmapMode::kNone)
        .value("Nearest", SkMipmapMode::kNearest)
        .value("Linear",  SkMipmapMode::kLinear);

    enum_<SkPaint::Style>("PaintStyle")
        .value("Fill",            SkPaint::Style::kFill_Style)
        .value("Stroke",          SkPaint::Style::kStroke_Style);

#ifdef SK_INCLUDE_PATHOPS
    enum_<SkPathOp>("PathOp")
        .value("Difference",         SkPathOp::kDifference_SkPathOp)
        .value("Intersect",          SkPathOp::kIntersect_SkPathOp)
        .value("Union",              SkPathOp::kUnion_SkPathOp)
        .value("XOR",                SkPathOp::kXOR_SkPathOp)
        .value("ReverseDifference",  SkPathOp::kReverseDifference_SkPathOp);
#endif

    enum_<SkCanvas::PointMode>("PointMode")
        .value("Points",   SkCanvas::PointMode::kPoints_PointMode)
        .value("Lines",    SkCanvas::PointMode::kLines_PointMode)
        .value("Polygon",  SkCanvas::PointMode::kPolygon_PointMode);

    enum_<SkPaint::Cap>("StrokeCap")
        .value("Butt",   SkPaint::Cap::kButt_Cap)
        .value("Round",  SkPaint::Cap::kRound_Cap)
        .value("Square", SkPaint::Cap::kSquare_Cap);

    enum_<SkPaint::Join>("StrokeJoin")
        .value("Miter", SkPaint::Join::kMiter_Join)
        .value("Round", SkPaint::Join::kRound_Join)
        .value("Bevel", SkPaint::Join::kBevel_Join);

#ifndef SK_NO_FONTS
    enum_<SkFontHinting>("FontHinting")
        .value("None",   SkFontHinting::kNone)
        .value("Slight", SkFontHinting::kSlight)
        .value("Normal", SkFontHinting::kNormal)
        .value("Full",   SkFontHinting::kFull);

    enum_<SkFont::Edging>("FontEdging")
#ifndef CANVASKIT_NO_ALIAS_FONT
        .value("Alias",             SkFont::Edging::kAlias)
#endif
        .value("AntiAlias",         SkFont::Edging::kAntiAlias)
        .value("SubpixelAntiAlias", SkFont::Edging::kSubpixelAntiAlias);
#endif

    enum_<SkTileMode>("TileMode")
        .value("Clamp",    SkTileMode::kClamp)
        .value("Repeat",   SkTileMode::kRepeat)
        .value("Mirror",   SkTileMode::kMirror)
        .value("Decal",    SkTileMode::kDecal);

    enum_<SkVertices::VertexMode>("VertexMode")
        .value("Triangles",       SkVertices::VertexMode::kTriangles_VertexMode)
        .value("TrianglesStrip",  SkVertices::VertexMode::kTriangleStrip_VertexMode)
        .value("TriangleFan",     SkVertices::VertexMode::kTriangleFan_VertexMode);

    // A value object is much simpler than a class - it is returned as a JS
    // object and does not require delete().
    // https://emscripten.org/docs/porting/connecting_cpp_and_javascript/embind.html#value-types

    value_object<SimpleImageInfo>("ImageInfo")
        .field("width",      &SimpleImageInfo::width)
        .field("height",     &SimpleImageInfo::height)
        .field("colorType",  &SimpleImageInfo::colorType)
        .field("alphaType",  &SimpleImageInfo::alphaType)
        .field("colorSpace", &SimpleImageInfo::colorSpace);

    value_object<StrokeOpts>("StrokeOpts")
        .field("width",       &StrokeOpts::width)
        .field("miter_limit", &StrokeOpts::miter_limit)
        .field("join",        &StrokeOpts::join)
        .field("cap",         &StrokeOpts::cap)
        .field("precision",   &StrokeOpts::precision);

    constant("MOVE_VERB",  MOVE);
    constant("LINE_VERB",  LINE);
    constant("QUAD_VERB",  QUAD);
    constant("CONIC_VERB", CONIC);
    constant("CUBIC_VERB", CUBIC);
    constant("CLOSE_VERB", CLOSE);

    constant("SaveLayerInitWithPrevious", (int)SkCanvas::SaveLayerFlagsSet::kInitWithPrevious_SaveLayerFlag);
    constant("SaveLayerF16ColorType",     (int)SkCanvas::SaveLayerFlagsSet::kF16ColorType);

    constant("ShadowTransparentOccluder", (int)SkShadowFlags::kTransparentOccluder_ShadowFlag);
    constant("ShadowGeometricOnly", (int)SkShadowFlags::kGeometricOnly_ShadowFlag);
    constant("ShadowDirectionalLight", (int)SkShadowFlags::kDirectionalLight_ShadowFlag);

    constant("_GlyphRunFlags_isWhiteSpace", (int)skia::textlayout::Paragraph::kWhiteSpace_VisitorFlag);
}