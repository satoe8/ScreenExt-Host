// InputInjector.h
// Turns TOUCH messages from the iPhone into real Windows touch input on the virtual monitor,
// using InitializeTouchInjection / InjectTouchInput (Windows 8+).
//
// Notes:
//  * Must run in the interactive user session. Injecting into elevated (admin) windows needs the
//    host to run elevated too (or UIAccess).
//  * InjectTouchInput takes ALL active contacts on every call, so we keep the active set and
//    resend it with the right per-contact state (DOWN / UPDATE / UP).
//  * The target rectangle is re-read from the display configuration on every event, so moving the
//    virtual monitor in Display Settings keeps working.
#pragma once

#include "Transport.h"   // TouchEvent (and winsock2 before windows.h)

#include <functional>
#include <map>
#include <mutex>

namespace host {

struct DesktopRect { LONG left = 0, top = 0, width = 0, height = 0; };
using RectProvider = std::function<bool(DesktopRect&)>;

class InputInjector {
public:
    bool Initialize(RectProvider provider);
    void OnTouch(const TouchEvent& t);   // any thread
    void ReleaseAll();                   // lift every finger (client disconnect / shutdown)

    uint64_t Injected() const { return injected_; }
    uint64_t Failed()   const { return failed_; }

private:
    enum class State { New, Update, Ending, Canceled };
    struct Contact { POINT pt; float pressure; State state; };

    bool InjectFrameLocked();

    RectProvider provider_;
    std::mutex   m_;
    std::map<uint8_t, Contact> active_;
    uint64_t injected_ = 0, failed_ = 0;
};

} // namespace host
