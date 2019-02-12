/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkShaper.h"
#include "SkSpan.h"
#include "SkTextBlobPriv.h"

SkShaper::RunHandler::Buffer SkTextBlobBuilderRunHandler::newRunBuffer(const RunInfo&,
                                                                       const SkFont& font,
                                                                       int glyphCount,
                                                                       SkSpan<const char> utf8) {
    const auto& runBuffer = SkTextBlobBuilderPriv::AllocRunTextPos(&fBuilder, font, glyphCount,
                                                                   utf8.size(), SkString());
    if (runBuffer.utf8text && fUtf8Text) {
        memcpy(runBuffer.utf8text, utf8.data(), utf8.size());
    }
    fClusters = runBuffer.clusters;
    fGlyphCount = glyphCount;
    fClusterOffset = utf8.data() - fUtf8Text;

    return { runBuffer.glyphs,
             runBuffer.points(),
             runBuffer.clusters };
}

void SkTextBlobBuilderRunHandler::commitRun() {
    for (int i = 0; i < fGlyphCount; ++i) {
        fClusters[i] -= fClusterOffset;
    }
}

sk_sp<SkTextBlob> SkTextBlobBuilderRunHandler::makeBlob() {
    return fBuilder.make();
}
