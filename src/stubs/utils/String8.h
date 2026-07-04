#pragma once
// Stub for android::String8 (from libutils)

namespace android {

class String8 {
    const char* m_str;
public:
    String8() : m_str("") {}
    String8(const char* s) : m_str(s) {}
    const char* string() const { return m_str; }
};

} // namespace android