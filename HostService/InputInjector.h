// InputInjector.h
// Turns TOUCH and PEN messages from the iPhone/iPad into real Windows input on the virtual monitor.
//
//   * fingers : InitializeTouchInjection / InjectTouchInput (Windows 8+); the full contact array is resent on every
//               event, with per-contact DOWN / UPDATE / UP state.
//   * Apple Pencil : a synthetic PEN device (CreateSyntheticPointerDevice + InjectSyntheticPointerInput, Windows 10
//               1809+, loaded dynamically) with pressure and tilt, so Windows Ink applications see a real active
//               pen. On older Windows the pencil degrades to a touch contact.
//   * tap slop: small jitter right after a contact lands is swallowed (and the lift snapped back to the landing
//               point) so Windows sees a clean TAP instead of a drag - the cause of "buttons don't click".
//   * palm rejection: while the pen is active (and briefly after), finger contacts are ignored on the host. (iPadOS
//               also rejects palms before touches ever reach the app.)
//
// Must run in the interactive user session. Injecting into elevated windows needs the host to run elevated.
#pragma once

#include "Transport.h"   // TouchEvent, PenEvent (and winsock2 before windows.h)

#include <functional>
#include <map>
#include <mutex>

namespace host {

struct DesktopRect { LONG left = 0, top = 0, width = 0, height = 0; };
using RectProvider = std::function<bool(DesktopRect&)>;

class InputInjector {
public:
    ~InputInjector();

    bool Initialize(RectProvider provider);          // touch (required) + pen (optional)
    void SetTapSlop(int pixels, int milliseconds);   // 0 pixels disables the filter
    void OnTouch(const TouchEvent& t);               // any thread
    void OnPen(const PenEvent& e);                   // any thread
    void ReleaseAll();                               // lift every finger and the pen (disconnect / shutdown)

    bool PenAvailable() const { return penDevice_ != nullptr; }
    uint64_t Injected() const { return injected_; }
    uint64_t Failed()   const { return failed_; }

private:
    enum class State { New, Update, Ending, Canceled };
    struct Contact {
        POINT pt; float pressure; State state;
        POINT downPt; ULONGLONG downTick; bool slop;   // tap-slop bookkeeping
    };

    bool ToDesktop(float nx, float ny, POINT& out);
    bool InjectTouchFrameLocked();
    bool InjectPenLocked(UINT32 flags, const POINT& pt, const PenEvent& e, bool contact);
    bool InitializePen();

    RectProvider provider_;
    std::mutex   m_;
    std::map<uint8_t, Contact> active_;
    uint64_t injected_ = 0, failed_ = 0;

    int slopPx_ = 8, slopMs_ = 150;

    // pen
    void*     penDevice_ = nullptr;      // HSYNTHETICPOINTERDEVICE
    bool      penFirstButton_ = true;    // set FIRSTBUTTON on contact (dropped automatically if Windows rejects it)
    bool      penDown_ = false, penSlop_ = false;
    POINT     penDownPt_{};
    ULONGLONG penDownTick_ = 0;
    ULONGLONG penLockUntil_ = 0;         // finger contacts are ignored until this tick (software palm rejection)
};

} // namespace host
