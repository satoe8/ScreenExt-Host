// InputInjector.cpp
#include "InputInjector.h"
#include "CaptureEngine.h"   // Logf

#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "user32.lib")

namespace host {

bool InputInjector::Initialize(RectProvider provider)
{
    provider_ = std::move(provider);
    // 10 simultaneous contacts, no on-screen touch visualisation (the finger is on the phone).
    if (!InitializeTouchInjection(10, TOUCH_FEEDBACK_NONE)) {
        Logf(L"[input] InitializeTouchInjection failed: %lu (needs Windows 8+ and the interactive session)\n",
             GetLastError());
        return false;
    }
    Logf(L"[input] Touch injection ready\n");
    return true;
}

bool InputInjector::InjectFrameLocked()
{
    std::vector<POINTER_TOUCH_INFO> frame;
    for (const auto& kv : active_) {
        const Contact& c = kv.second;
        POINTER_TOUCH_INFO ti{};
        ti.pointerInfo.pointerType = PT_TOUCH;
        ti.pointerInfo.pointerId = kv.first;
        ti.pointerInfo.ptPixelLocation = c.pt;
        switch (c.state) {
        case State::New:      ti.pointerInfo.pointerFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN; break;
        case State::Update:   ti.pointerInfo.pointerFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE; break;
        case State::Ending:   ti.pointerInfo.pointerFlags = POINTER_FLAG_UP; break;
        case State::Canceled: ti.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED; break;
        }
        ti.touchFlags = TOUCH_FLAG_NONE;
        ti.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
        ti.rcContact = { c.pt.x - 4, c.pt.y - 4, c.pt.x + 4, c.pt.y + 4 };
        ti.orientation = 90;
        ti.pressure = (UINT32)std::min(1024.0f, std::max(1.0f, c.pressure * 1024.0f));
        frame.push_back(ti);
    }
    if (frame.empty()) return true;

    // ERROR_NOT_READY = calls too close together; the docs say to retry the same frame.
    DWORD err = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (InjectTouchInput((UINT32)frame.size(), frame.data())) { ++injected_; return true; }
        err = GetLastError();
        if (err != ERROR_NOT_READY) break;
        Sleep(1);
    }
    ++failed_;
    if (failed_ <= 5 || (failed_ % 100) == 0)
        Logf(L"[input] InjectTouchInput failed: %lu (total failures %llu)\n", err, (unsigned long long)failed_);
    return false;
}

void InputInjector::OnTouch(const TouchEvent& t)
{
    DesktopRect r;
    if (!provider_ || !provider_(r) || r.width <= 0 || r.height <= 0)
        return;

    const float nx = std::min(1.0f, std::max(0.0f, t.x));
    const float ny = std::min(1.0f, std::max(0.0f, t.y));
    const POINT pt = { r.left + (LONG)std::lround(nx * (r.width - 1)),
                       r.top  + (LONG)std::lround(ny * (r.height - 1)) };

    std::lock_guard<std::mutex> lk(m_);

    switch (t.phase) {
    case TouchPhase::Down:
        active_[t.pointerId] = { pt, t.pressure, State::New };
        break;
    case TouchPhase::Move: {
        auto it = active_.find(t.pointerId);
        if (it == active_.end()) {                       // missed the DOWN: start the contact now
            active_[t.pointerId] = { pt, t.pressure, State::New };
        } else {
            if (it->second.pt.x == pt.x && it->second.pt.y == pt.y) return;   // nothing changed
            it->second.pt = pt;
            it->second.pressure = t.pressure;
        }
        break;
    }
    case TouchPhase::Up:
    case TouchPhase::Cancel: {
        auto it = active_.find(t.pointerId);
        if (it == active_.end()) return;
        it->second.pt = pt;
        it->second.state = (t.phase == TouchPhase::Up) ? State::Ending : State::Canceled;
        break;
    }
    }

    const bool ok = InjectFrameLocked();

    if (!ok) {
        active_.clear();   // unknown injector state: start clean rather than leave stuck contacts
        return;
    }
    for (auto it = active_.begin(); it != active_.end();) {
        if (it->second.state == State::Ending || it->second.state == State::Canceled) it = active_.erase(it);
        else { if (it->second.state == State::New) it->second.state = State::Update; ++it; }
    }
}

void InputInjector::ReleaseAll()
{
    std::lock_guard<std::mutex> lk(m_);
    if (active_.empty()) return;
    for (auto& kv : active_) kv.second.state = State::Canceled;
    InjectFrameLocked();
    active_.clear();
}

} // namespace host
