/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/GrPipeline.h"

#include "src/gpu/GrAppliedClip.h"
#include "src/gpu/GrCaps.h"
#include "src/gpu/GrGpu.h"
#include "src/gpu/GrRenderTargetContext.h"
#include "src/gpu/GrXferProcessor.h"

#include "src/gpu/ops/GrOp.h"

GrPipeline::GrPipeline(const InitArgs& args,
                       sk_sp<const GrXferProcessor> xferProcessor,
                       const GrAppliedHardClip& hardClip)
        : fWriteSwizzle(args.fWriteSwizzle) {
    fFlags = (Flags)args.fInputFlags;
    if (hardClip.hasStencilClip()) {
        fFlags |= Flags::kHasStencilClip;
    }
    if (hardClip.scissorState().enabled()) {
        fFlags |= Flags::kScissorTestEnabled;
    }

    fWindowRectsState = hardClip.windowRectsState();
    this->setUserStencil(args.fUserStencil);

    fXferProcessor = std::move(xferProcessor);

    if (args.fDstProxyView.proxy()) {
        fDstProxyView = args.fDstProxyView.proxyView();
        fDstTextureOffset = args.fDstProxyView.offset();
    }
}

GrPipeline::GrPipeline(const InitArgs& args, GrProcessorSet&& processors,
                       GrAppliedClip&& appliedClip)
        : GrPipeline(args, processors.refXferProcessor(), appliedClip.hardClip()) {
    SkASSERT(processors.isFinalized());
    // Copy GrFragmentProcessors from GrProcessorSet to Pipeline
    fNumColorProcessors = processors.hasColorFragmentProcessor() ? 1 : 0;
    int numTotalProcessors = fNumColorProcessors +
                             (processors.hasCoverageFragmentProcessor() ? 1 : 0) +
                             (appliedClip.hasCoverageFragmentProcessor() ? 1 : 0);
    fFragmentProcessors.reset(numTotalProcessors);

    int currFPIdx = 0;
    if (processors.hasColorFragmentProcessor()) {
        fFragmentProcessors[currFPIdx++] = processors.detachColorFragmentProcessor();
    }
    if (processors.hasCoverageFragmentProcessor()) {
        fFragmentProcessors[currFPIdx++] = processors.detachCoverageFragmentProcessor();
    }
    if (appliedClip.hasCoverageFragmentProcessor()) {
        fFragmentProcessors[currFPIdx++] = appliedClip.detachCoverageFragmentProcessor();
    }
}

GrXferBarrierType GrPipeline::xferBarrierType(GrTexture* texture, const GrCaps& caps) const {
    auto proxy = fDstProxyView.proxy();
    if (proxy && proxy->peekTexture() == texture) {
        return kTexture_GrXferBarrierType;
    }
    return this->getXferProcessor().xferBarrierType(caps);
}

GrPipeline::GrPipeline(GrScissorTest scissorTest,
                       sk_sp<const GrXferProcessor> xp,
                       const GrSwizzle& writeSwizzle,
                       InputFlags inputFlags,
                       const GrUserStencilSettings* userStencil)
        : fWindowRectsState()
        , fFlags((Flags)inputFlags)
        , fXferProcessor(std::move(xp))
        , fWriteSwizzle(writeSwizzle) {
    if (GrScissorTest::kEnabled == scissorTest) {
        fFlags |= Flags::kScissorTestEnabled;
    }
    this->setUserStencil(userStencil);
}

void GrPipeline::genKey(GrProcessorKeyBuilder* b, const GrCaps& caps) const {
    // kSnapVerticesToPixelCenters is implemented in a shader.
    InputFlags ignoredFlags = InputFlags::kSnapVerticesToPixelCenters;
    if (!caps.multisampleDisableSupport()) {
        // Ganesh will omit kHWAntialias regardless of multisampleDisableSupport.
        ignoredFlags |= InputFlags::kHWAntialias;
    }
    b->add32((uint32_t)fFlags & ~(uint32_t)ignoredFlags);

    const GrXferProcessor::BlendInfo& blendInfo = this->getXferProcessor().getBlendInfo();

    static constexpr uint32_t kBlendWriteShift = 1;
    static constexpr uint32_t kBlendCoeffShift = 5;
    static_assert(kLast_GrBlendCoeff < (1 << kBlendCoeffShift));
    static_assert(kFirstAdvancedGrBlendEquation - 1 < 4);

    uint32_t blendKey = blendInfo.fWriteColor;
    blendKey |= (blendInfo.fSrcBlend << kBlendWriteShift);
    blendKey |= (blendInfo.fDstBlend << (kBlendWriteShift + kBlendCoeffShift));
    blendKey |= (blendInfo.fEquation << (kBlendWriteShift + 2 * kBlendCoeffShift));

    b->add32(blendKey);
}

void GrPipeline::visitTextureEffects(
        const std::function<void(const GrTextureEffect&)>& func) const {
    for (auto& fp : fFragmentProcessors) {
        fp->visitTextureEffects(func);
    }
}

void GrPipeline::visitProxies(const GrOp::VisitProxyFunc& func) const {
    // This iteration includes any clip coverage FPs
    for (auto& fp : fFragmentProcessors) {
        fp->visitProxies(func);
    }
    if (fDstProxyView.asTextureProxy()) {
        func(fDstProxyView.asTextureProxy(), GrMipmapped::kNo);
    }
}
