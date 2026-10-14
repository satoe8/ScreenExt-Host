// UsbmuxdClient.h
// Minimal client for Apple's usbmuxd (on Windows: "Apple Mobile Device Service", installed by iTunes or
// the "Apple Devices" Microsoft Store app), listening on TCP 127.0.0.1:27015.
//
// This replaces `iproxy`: it finds a plugged-in iPhone/iPad and opens a tunnel to a TCP port ON the device,
// where the iOS app is listening. Protocol (verified against several independent implementations):
//   * every packet = 16-byte header of four little-endian u32 {length incl. header, version = 1 (plist),
//     request = 8, tag} followed by an XML plist
//   * one request per TCP connection: ListDevices returns {DeviceList: [...]}; Connect replies
//     {MessageType: Result, Number: n} and, if n == 0, the SAME socket becomes the raw tunnel
//   * Connect's PortNumber is byte-swapped (network order): port 5000 is sent as 34835
//   * Result numbers: 0 ok, 2 bad device, 3 connection refused (nothing listening on that device port)
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace host {

struct UsbDevice {
    uint32_t    deviceId = 0;   // muxer-assigned id, valid while the device stays plugged in
    std::string serial;         // UDID / serial number
};

class UsbmuxdClient {
public:
    static constexpr uint16_t kMuxPort = 27015;

    // Lists attached devices that are connected over USB (Wi-Fi-sync "Network" entries are skipped).
    // Returns false if the muxer can't be reached (Apple Mobile Device Service missing/stopped).
    static bool ListDevices(std::vector<UsbDevice>& out);

    // Opens a tunnel to `devicePort` on the device. On success returns a connected, blocking SOCKET that is
    // a raw byte pipe to the app (no timeouts set). On failure returns INVALID_SOCKET and, if `result` is
    // non-null, stores the muxer's result code (3 = nothing listening on that port) or -1 for I/O errors.
    // Winsock must already be initialised (WSAStartup).
    static SOCKET ConnectToDevice(uint32_t deviceId, uint16_t devicePort, int* result = nullptr);
};

} // namespace host
