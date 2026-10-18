// InputInjector.cpp
#include "InputInjector.h"
#include "CaptureEngine.h"   // Logf

#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "user32.lib")

namespace host {

namespace {

// CreateSyntheticPointerDevice & co. exist from Windows 10 1809; load them dynamically so the host still runs
// (with pen degraded to touch) on older builds and doesn't depend on the SDK version.
using PFN_CreateDevice  = void* (WINAPI*)(POINTER_INPUT_TYPE, ULONG, UINT /*POINTER_FEEDBACK_MODE*/);
using PFN_InjectPointer = BOOL  (WINAPI*)(void*, const POINTER_TYPE_INFO*, UINT32);
using PFN_DestroyDevice = void  (WINAPI*)(void*);

PFN_CreateDevice  g_createDevice  = nullptr;
PFN_InjectPointer g_injectPointer = nullptr;
PFN_DestroyDevice g_destroyDevice = nullptr;

constexpr UINT kFeedbackNone = 3;   // POINTER_FEEDBACK_NONE

} // namespace

InputInjector::~InputInjector()
{
    if (penDevice_ && g_destroyDevice) g_destroyDevice(penDevice_);
}

bool InputInjector::InitializePen()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;
    g_createDevice  = reinterpret_cast<PFN_CreateDevice>(GetProcAddress(user32, "CreateSyntheticPointerDevice"));
    g_injectPointer = reinterpret_cast<PFN_InjectPointer>(GetProcAddress(user32, "InjectSyntheticPointerInput"));
    g_destroyDevice = reinterpret_cast<PFN_DestroyDevice>(GetProcAddress(user32, "DestroySyntheticPointerDevice"));
    if (!g_createDevice || !g_injectPointer) {
        Logf(L"[input] Pen injection needs Windows 10 1809+; the Apple Pencil will act as a finger\n");
        return false;
    }
    penDevice_ = g_createDevice(PT_PEN, 1, kFeedbackNone);
    if (!penDevice_) {
        Logf(L"[input] CreateSyntheticPointerDevice(PT_PEN) failed: %lu; the Apple Pencil will act as a finger\n", GetLastError());
        return false;
    }
    Logf(L"[input] Pen injection ready (Windows Ink: pressure + tilt)\n");
    return true;
}

bool InputInjector::Initialize(RectProvider provider)
{
    provider_ = std::move(provider);
    InitializePen();   // optional

    // 10 simultaneous finger contacts, no on-screen touch visualisation (the finger is on the tablet).
    if (!InitializeTouchInjection(10, TOUCH_FEEDBACK_NONE)) {
        Logf(L"[input] InitializeTouchInjection failed: %lu (needs Windows 8+ and the interactive session)\n", GetLastError());
        return false;
    }
    Logf(L"[input] Touch injection ready (tap slop %d px / %d ms)\n", slopPx_, slopMs_);
    return true;
}

void InputInjector::SetTapSlop(int pixels, int milliseconds)
{
    std::lock_guard<std::mutex> lk(m_);
    slopPx_ = std::max(0, pixels);
    slopMs_ = std::max(0, milliseconds);
}

bool InputInjector::ToDesktop(float nx, float ny, POINT& out)
{
    DesktopRect r;
    if (!provider_ || !provider_(r) || r.width <= 0 || r.height <= 0) return false;
    nx = std::min(1.0f, std::max(0.0f, nx));
    ny = std::min(1.0f, std::max(0.0f, ny));
    out.x = r.left + (LONG)std::lround(nx * (r.width - 1));
    out.y = r.top  + (LONG)std::lround(ny * (r.height - 1));
    return true;
}

// ------------------------------------------------------------------------------------------------
// Fingers
// ------------------------------------------------------------------------------------------------

bool InputInjector::InjectTouchFrameLocked()
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
    POINT pt;
    if (!ToDesktop(t.x, t.y, pt)) return;
    const ULONGLONG now = GetTickCount64();

    std::lock_guard<std::mutex> lk(m_);

    // Software palm rejection: a finger that lands while the pen is active (or just lifted) is ignored.
    if (now < penLockUntil_ && active_.find(t.pointerId) == active_.end())
        return;

    switch (t.phase) {
    case TouchPhase::Down:
        active_[t.pointerId] = { pt, t.pressure, State::New, pt, now, slopPx_ > 0 };
        break;

    case TouchPhase::Move: {
        auto it = active_.find(t.pointerId);
        if (it == active_.end()) {                       // missed the DOWN: start the contact now
            active_[t.pointerId] = { pt, t.pressure, State::New, pt, now, slopPx_ > 0 };
            break;
        }
        Contact& c = it->second;
        if (c.slop) {
            // Tap slop: ignore wobble near the landing point during the first moments of contact.
            const int dx = pt.x - c.downPt.x, dy = pt.y - c.downPt.y;
            const bool inside = (dx * dx + dy * dy) <= slopPx_ * slopPx_;
            if (inside && (now - c.downTick) < (ULONGLONG)slopMs_) return;
            c.slop = false;                              // real movement: follow the finger from now on
        }
        if (c.pt.x == pt.x && c.pt.y == pt.y) return;    // nothing changed
        c.pt = pt;
        c.pressure = t.pressure;
        break;
    }

    case TouchPhase::Up:
    case TouchPhase::Cancel: {
        auto it = active_.find(t.pointerId);
        if (it == active_.end()) return;
        Contact& c = it->second;
        // A lift that never left the slop circle is reported AT the landing point: a perfect tap.
        c.pt = (c.slop && (now - c.downTick) < (ULONGLONG)slopMs_) ? c.downPt : pt;
        c.state = (t.phase == TouchPhase::Up) ? State::Ending : State::Canceled;
        break;
    }
    }

    if (!InjectTouchFrameLocked()) {
        active_.clear();   // unknown injector state: start clean rather than leave stuck contacts
        return;
    }
    for (auto it = active_.begin(); it != active_.end();) {
        if (it->second.state == State::Ending || it->second.state == State::Canceled) it = active_.erase(it);
        else { if (it->second.state == State::New) it->second.state = State::Update; ++it; }
    }
}

// ------------------------------------------------------------------------------------------------
// Apple Pencil -> Windows pen
// ------------------------------------------------------------------------------------------------

bool InputInjector::InjectPenLocked(UINT32 flags, const POINT& pt, const PenEvent& e, bool contact)
{
    POINTER_TYPE_INFO pti{};
    pti.type = PT_PEN;
    POINTER_PEN_INFO& pen = pti.penInfo;
    pen.pointerInfo.pointerType = PT_PEN;
    pen.pointerInfo.pointerId = 0;
    pen.pointerInfo.ptPixelLocation = pt;
    pen.penFlags = PEN_FLAG_NONE;
    if (e.barrel) pen.penFlags |= PEN_FLAG_BARREL;
    if (e.eraser) pen.penFlags |= PEN_FLAG_ERASER | PEN_FLAG_INVERTED;
    pen.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    pen.pressure = contact ? (UINT32)std::min(1024.0f, std::max(1.0f, e.pressure * 1024.0f)) : 0;
    pen.tiltX = e.tiltX;
    pen.tiltY = e.tiltY;

    DWORD err = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        pen.pointerInfo.pointerFlags = flags | ((contact && penFirstButton_) ? POINTER_FLAG_FIRSTBUTTON : 0);
        if (g_injectPointer(penDevice_, &pti, 1)) { ++injected_; return true; }
        err = GetLastError();
        if (err == ERROR_NOT_READY) { Sleep(1); continue; }
        if (err == ERROR_INVALID_PARAMETER && penFirstButton_ && contact) {   // this build dislikes FIRSTBUTTON
            penFirstButton_ = false;
            continue;
        }
        break;
    }
    ++failed_;
    if (failed_ <= 5 || (failed_ % 100) == 0)
        Logf(L"[input] InjectSyntheticPointerInput(pen) failed: %lu (total failures %llu)\n", err, (unsigned long long)failed_);
    return false;
}

void InputInjector::OnPen(const PenEvent& e)
{
    // Older Windows: fall back to a finger contact (no pressure/tilt).
    if (!penDevice_) {
        OnTouch(TouchEvent{ 15, e.phase, e.x, e.y, e.pressure });
        return;
    }

    POINT pt;
    if (!ToDesktop(e.x, e.y, pt)) return;
    const ULONGLONG now = GetTickCount64();

    std::lock_guard<std::mutex> lk(m_);
    penLockUntil_ = now + 350;       // palm rejection window: fingers are ignored while the pen is busy

    const UINT32 inContact = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;

    switch (e.phase) {
    case TouchPhase::Down:
        penDown_ = true; penSlop_ = slopPx_ > 0; penDownPt_ = pt; penDownTick_ = now;
        InjectPenLocked(inContact | POINTER_FLAG_DOWN, pt, e, true);
        break;

    case TouchPhase::Move:
        if (!penDown_) {             // missed the DOWN
            penDown_ = true; penSlop_ = slopPx_ > 0; penDownPt_ = pt; penDownTick_ = now;
            InjectPenLocked(inContact | POINTER_FLAG_DOWN, pt, e, true);
            break;
        }
        if (penSlop_) {
            const int dx = pt.x - penDownPt_.x, dy = pt.y - penDownPt_.y;
            const bool inside = (dx * dx + dy * dy) <= slopPx_ * slopPx_;
            if (inside && (now - penDownTick_) < (ULONGLONG)slopMs_) {
                // Still within the tap window: keep pressure/tilt flowing but don't move the pointer.
                InjectPenLocked(inContact | POINTER_FLAG_UPDATE, penDownPt_, e, true);
                break;
            }
            penSlop_ = false;
        }
        InjectPenLocked(inContact | POINTER_FLAG_UPDATE, pt, e, true);
        break;

    case TouchPhase::Up:
    case TouchPhase::Cancel: {
        if (!penDown_) break;
        const POINT upPt = (penSlop_ && (now - penDownTick_) < (ULONGLONG)slopMs_) ? penDownPt_ : pt;
        penDown_ = false;
        UINT32 f = POINTER_FLAG_UP | POINTER_FLAG_INRANGE;
        if (e.phase == TouchPhase::Cancel) f |= POINTER_FLAG_CANCELED;
        InjectPenLocked(f, upPt, e, false);
        InjectPenLocked(POINTER_FLAG_UPDATE, upPt, e, false);   // pen leaves detection range
        break;
    }
    }
}

void InputInjector::ReleaseAll()
{
    std::lock_guard<std::mutex> lk(m_);
    if (!active_.empty()) {
        for (auto& kv : active_) kv.second.state = State::Canceled;
        InjectTouchFrameLocked();
        active_.clear();
    }
    if (penDevice_ && penDown_) {
        PenEvent e{ TouchPhase::Cancel, false, false, 0, 0, 0, 0, 0, 0 };
        POINT pt = penDownPt_;
        InjectPenLocked(POINTER_FLAG_UP | POINTER_FLAG_INRANGE | POINTER_FLAG_CANCELED, pt, e, false);
        InjectPenLocked(POINTER_FLAG_UPDATE, pt, e, false);
        penDown_ = false;
    }
}

} // namespace host
