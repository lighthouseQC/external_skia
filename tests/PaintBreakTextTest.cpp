/*
 * Copyright 2011-2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkAutoMalloc.h"
#include "SkFont.h"
#include "Test.h"

#ifdef SK_SUPPORT_LEGACY_BREAKTEXT
static void test_monotonic(skiatest::Reporter* reporter, const SkFont& font, const char* msg) {
    const char* text = "sdfkljAKLDFJKEWkldfjlk#$%&sdfs.dsj";
    const size_t length = strlen(text);
    const SkScalar width = font.measureText(text, length, kUTF8_SkTextEncoding);

    SkScalar mm = 0;
    size_t nn = 0;
    const SkScalar step = SkMaxScalar(width / 10, SK_Scalar1);
    for (SkScalar w = 0; w <= width; w += step) {
        SkScalar m;
        const size_t n = font.breakText(text, length, kUTF8_SkTextEncoding, w, &m);

        REPORTER_ASSERT(reporter, n <= length, msg);
        REPORTER_ASSERT(reporter, m <= width, msg);

        if (n == 0) {
            REPORTER_ASSERT(reporter, m == 0, msg);
        } else {
            // now assert that we're monotonic
            if (n == nn) {
                REPORTER_ASSERT(reporter, m == mm, msg);
            } else {
                REPORTER_ASSERT(reporter, n > nn, msg);
                REPORTER_ASSERT(reporter, m > mm, msg);
            }
        }
        nn = n;
        mm = m;
    }
}

static void test_eq_measure_text(skiatest::Reporter* reporter, const SkFont& font,
                                 const char* msg) {
    const char* text = "The ultimate measure of a man is not where he stands in moments of comfort "
        "and convenience, but where he stands at times of challenge and controversy.";
    const size_t length = strlen(text);
    const SkScalar width = font.measureText(text, length, kUTF8_SkTextEncoding);

    SkScalar mm;
    const size_t length2 = font.breakText(text, length, kUTF8_SkTextEncoding, width, &mm);
    REPORTER_ASSERT(reporter, length2 == length, msg);
    REPORTER_ASSERT(reporter, mm == width, msg);
}

static void test_long_text(skiatest::Reporter* reporter, const SkFont& font, const char* msg) {
    static const int kSize = 16 * 1024;
    SkAutoMalloc block(kSize);
    memset(block.get(), 'a', kSize - 1);
    char* text = static_cast<char*>(block.get());
    text[kSize - 1] = '\0';
    const SkScalar width = font.measureText(text, kSize, kUTF8_SkTextEncoding);

    SkScalar mm;
    const size_t length = font.breakText(text, kSize, kUTF8_SkTextEncoding, width, &mm);
    REPORTER_ASSERT(reporter, length == kSize, msg);
    REPORTER_ASSERT(reporter, mm == width, msg);
}

DEF_TEST(PaintBreakText, reporter) {
    SkFont font;
    test_monotonic(reporter, font, "default");
    test_eq_measure_text(reporter, font, "default");
    test_long_text(reporter, font, "default");
    font.setSize(SkIntToScalar(1 << 17));
    test_monotonic(reporter, font, "huge text size");
    test_eq_measure_text(reporter, font, "huge text size");
    font.setSize(0);
    test_monotonic(reporter, font, "zero text size");
}
#endif
