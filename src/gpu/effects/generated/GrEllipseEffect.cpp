/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/**************************************************************************************************
 *** This file was autogenerated from GrEllipseEffect.fp; do not modify.
 **************************************************************************************************/
#include "GrEllipseEffect.h"

#include "src/core/SkUtils.h"
#include "src/gpu/GrTexture.h"
#include "src/gpu/glsl/GrGLSLFragmentProcessor.h"
#include "src/gpu/glsl/GrGLSLFragmentShaderBuilder.h"
#include "src/gpu/glsl/GrGLSLProgramBuilder.h"
#include "src/sksl/SkSLCPP.h"
#include "src/sksl/SkSLUtil.h"
class GrGLSLEllipseEffect : public GrGLSLFragmentProcessor {
public:
    GrGLSLEllipseEffect() {}
    void emitCode(EmitArgs& args) override {
        GrGLSLFPFragmentBuilder* fragBuilder = args.fFragBuilder;
        const GrEllipseEffect& _outer = args.fFp.cast<GrEllipseEffect>();
        (void)_outer;
        auto edgeType = _outer.edgeType;
        (void)edgeType;
        auto center = _outer.center;
        (void)center;
        auto radii = _outer.radii;
        (void)radii;
        prevRadii = float2(-1.0);
        medPrecision = !sk_Caps.floatIs32Bits;
        ellipseVar = args.fUniformHandler->addUniform(&_outer, kFragment_GrShaderFlag,
                                                      kFloat4_GrSLType, "ellipse");
        if (medPrecision) {
            scaleVar = args.fUniformHandler->addUniform(&_outer, kFragment_GrShaderFlag,
                                                        kFloat2_GrSLType, "scale");
        }
        fragBuilder->codeAppendf(
                R"SkSL(float2 prevCenter;
float2 prevRadii = float2(%f, %f);
bool medPrecision = %s;
float2 d = sk_FragCoord.xy - %s.xy;
@if (medPrecision) {
    d *= %s.y;
}
float2 Z = d * %s.zw;
float implicit = dot(Z, d) - 1.0;
float grad_dot = 4.0 * dot(Z, Z);
@if (medPrecision) {
    grad_dot = max(grad_dot, 6.1036000261083245e-05);
} else {
    grad_dot = max(grad_dot, 1.1754999560161448e-38);
}
float approx_dist = implicit * inversesqrt(grad_dot);
@if (medPrecision) {
    approx_dist *= %s.x;
}
half alpha;
@switch (%d) {
    case 0:
        alpha = approx_dist > 0.0 ? 0.0 : 1.0;
        break;
    case 1:
        alpha = clamp(0.5 - half(approx_dist), 0.0, 1.0);
        break;
    case 2:
        alpha = approx_dist > 0.0 ? 1.0 : 0.0;
        break;
    case 3:
        alpha = clamp(0.5 + half(approx_dist), 0.0, 1.0);
        break;
    default:
        discard;
})SkSL",
                prevRadii.fX, prevRadii.fY, (medPrecision ? "true" : "false"),
                args.fUniformHandler->getUniformCStr(ellipseVar),
                scaleVar.isValid() ? args.fUniformHandler->getUniformCStr(scaleVar) : "float2(0)",
                args.fUniformHandler->getUniformCStr(ellipseVar),
                scaleVar.isValid() ? args.fUniformHandler->getUniformCStr(scaleVar) : "float2(0)",
                (int)_outer.edgeType);
        SkString _sample0 = this->invokeChild(0, args);
        fragBuilder->codeAppendf(
                R"SkSL(
return %s * alpha;
)SkSL",
                _sample0.c_str());
    }

private:
    void onSetData(const GrGLSLProgramDataManager& pdman,
                   const GrFragmentProcessor& _proc) override {
        const GrEllipseEffect& _outer = _proc.cast<GrEllipseEffect>();
        auto edgeType = _outer.edgeType;
        (void)edgeType;
        auto center = _outer.center;
        (void)center;
        auto radii = _outer.radii;
        (void)radii;
        UniformHandle& ellipse = ellipseVar;
        (void)ellipse;
        UniformHandle& scale = scaleVar;
        (void)scale;

        if (radii != prevRadii || center != prevCenter) {
            float invRXSqd;
            float invRYSqd;
            // If we're using a scale factor to work around precision issues, choose the larger
            // radius as the scale factor. The inv radii need to be pre-adjusted by the scale
            // factor.
            if (scale.isValid()) {
                if (radii.fX > radii.fY) {
                    invRXSqd = 1.f;
                    invRYSqd = (radii.fX * radii.fX) / (radii.fY * radii.fY);
                    pdman.set2f(scale, radii.fX, 1.f / radii.fX);
                } else {
                    invRXSqd = (radii.fY * radii.fY) / (radii.fX * radii.fX);
                    invRYSqd = 1.f;
                    pdman.set2f(scale, radii.fY, 1.f / radii.fY);
                }
            } else {
                invRXSqd = 1.f / (radii.fX * radii.fX);
                invRYSqd = 1.f / (radii.fY * radii.fY);
            }
            pdman.set4f(ellipse, center.fX, center.fY, invRXSqd, invRYSqd);
            prevCenter = center;
            prevRadii = radii;
        }
    }
    SkPoint prevCenter = float2(0);
    SkPoint prevRadii = float2(0);
    bool medPrecision = false;
    UniformHandle ellipseVar;
    UniformHandle scaleVar;
};
GrGLSLFragmentProcessor* GrEllipseEffect::onCreateGLSLInstance() const {
    return new GrGLSLEllipseEffect();
}
void GrEllipseEffect::onGetGLSLProcessorKey(const GrShaderCaps& caps,
                                            GrProcessorKeyBuilder* b) const {
    b->add32((uint32_t)edgeType);
}
bool GrEllipseEffect::onIsEqual(const GrFragmentProcessor& other) const {
    const GrEllipseEffect& that = other.cast<GrEllipseEffect>();
    (void)that;
    if (edgeType != that.edgeType) return false;
    if (center != that.center) return false;
    if (radii != that.radii) return false;
    return true;
}
bool GrEllipseEffect::usesExplicitReturn() const { return true; }
GrEllipseEffect::GrEllipseEffect(const GrEllipseEffect& src)
        : INHERITED(kGrEllipseEffect_ClassID, src.optimizationFlags())
        , edgeType(src.edgeType)
        , center(src.center)
        , radii(src.radii) {
    this->cloneAndRegisterAllChildProcessors(src);
}
std::unique_ptr<GrFragmentProcessor> GrEllipseEffect::clone() const {
    return std::make_unique<GrEllipseEffect>(*this);
}
#if GR_TEST_UTILS
SkString GrEllipseEffect::onDumpInfo() const {
    return SkStringPrintf("(edgeType=%d, center=float2(%f, %f), radii=float2(%f, %f))",
                          (int)edgeType, center.fX, center.fY, radii.fX, radii.fY);
}
#endif
GR_DEFINE_FRAGMENT_PROCESSOR_TEST(GrEllipseEffect);
#if GR_TEST_UTILS
std::unique_ptr<GrFragmentProcessor> GrEllipseEffect::TestCreate(GrProcessorTestData* testData) {
    SkPoint center;
    center.fX = testData->fRandom->nextRangeScalar(0.f, 1000.f);
    center.fY = testData->fRandom->nextRangeScalar(0.f, 1000.f);
    SkScalar rx = testData->fRandom->nextRangeF(0.f, 1000.f);
    SkScalar ry = testData->fRandom->nextRangeF(0.f, 1000.f);
    bool success;
    std::unique_ptr<GrFragmentProcessor> fp = testData->inputFP();
    do {
        GrClipEdgeType et = (GrClipEdgeType)testData->fRandom->nextULessThan(kGrClipEdgeTypeCnt);
        std::tie(success, fp) = GrEllipseEffect::Make(
                std::move(fp), et, center, SkPoint::Make(rx, ry), *testData->caps()->shaderCaps());
    } while (!success);
    return fp;
}
#endif
