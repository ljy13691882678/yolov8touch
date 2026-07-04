#pragma once
// Stub for android::Surface (from libgui)

#include <utils/RefBase.h>
#include <android/native_window.h>

namespace android {

class Surface : public RefBase {
public:
    Surface() {}
    operator ANativeWindow*() const { return nullptr; }
};

} // namespace android