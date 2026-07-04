#pragma once
// Stub for android::SurfaceControl (from libgui)

#include <utils/RefBase.h>
#include <gui/Surface.h>

namespace android {

class SurfaceControl : public RefBase {
public:
    SurfaceControl() {}
    sp<Surface> getSurface() { return sp<Surface>(new Surface()); }
};

} // namespace android