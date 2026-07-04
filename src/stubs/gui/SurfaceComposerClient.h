#pragma once
// Stub for android::SurfaceComposerClient (from libgui)

#include <cstdint>
#include <utils/RefBase.h>
#include <utils/String8.h>
#include <gui/SurfaceControl.h>

namespace android {

typedef int32_t status_t;

enum {
    NO_ERROR = 0,
};

enum {
    PIXEL_FORMAT_RGBA_8888 = 1,
};

class SurfaceComposerClient : public RefBase {
public:
    SurfaceComposerClient() {}
    status_t initCheck() const { return NO_ERROR; }

    sp<SurfaceControl> createSurface(
        const String8& /*name*/,
        uint32_t /*w*/,
        uint32_t /*h*/,
        int32_t /*format*/,
        uint32_t /*flags*/)
    {
        return sp<SurfaceControl>(new SurfaceControl());
    }

    class Transaction {
    public:
        Transaction() {}

        Transaction& setLayer(const sp<SurfaceControl>&, int32_t) { return *this; }
        Transaction& setAlpha(const sp<SurfaceControl>&, float) { return *this; }
        Transaction& show(const sp<SurfaceControl>&) { return *this; }
        Transaction& hide(const sp<SurfaceControl>&) { return *this; }
        void apply() {}
    };
};

} // namespace android