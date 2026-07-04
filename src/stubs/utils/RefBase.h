#pragma once
// Stub for android::sp<> and RefBase (from libutils)

namespace android {

class RefBase {
public:
    RefBase() {}
    virtual ~RefBase() {}
};

template<typename T>
class sp {
    T* m_ptr;
public:
    sp() : m_ptr(nullptr) {}
    sp(T* p) : m_ptr(p) {}
    sp(const sp<T>& o) : m_ptr(o.m_ptr) {}
    ~sp() {}

    sp<T>& operator=(T* p) { m_ptr = p; return *this; }
    T* get() const { return m_ptr; }
    T& operator*() const { return *m_ptr; }
    T* operator->() const { return m_ptr; }
    operator bool() const { return m_ptr != nullptr; }
    void clear() { m_ptr = nullptr; }
};

} // namespace android