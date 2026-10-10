# VNC Device Link: local serial ports, network devices and files for the desktop

Status: **draft 1**, for discussion with the TigerVNC developers.
Pseudo-encoding and message type: to be assigned (see
[Registration](#registration)).

## Summary

A point of sale often has small devices next to the screen: a customer
display, a card reader, a payment terminal on a serial line or in the local
network, or a program that writes its results into a file. When the
application runs on a remote desktop and the user sees it through a VNC
viewer, these devices are on the wrong side.

With this proposal the viewer offers named devices to the session, for
example `display`, `cardreader` or `payment`. Which local port, address or
file stands behind a name is configured in the viewer only. An application on
the remote desktop opens a device by name; the bytes travel inside the RFB
connection in both directions, together with the settings of a serial line
(speed, format, flow control, modem lines).

Nothing listens on the viewer's machine, and the remote desktop never learns
or chooses a local path.

## Why inside RFB

[VNC File Drop](file-drop.md) moves files over a side channel, because large
transfers would block the session and need resuming. Device links are the
opposite case:

- **Little data, short delays.** A customer display gets a few bytes per sale,
  a payment terminal a few hundred per transaction. What matters is that they
  arrive quickly and in order, over whatever path the RFB connection already
  takes (TLS, SSH, VPN, NAT).
- **No second port.** A side channel needs its own port, certificate and
  firewall rule on the server; RFB is already there.
- **No connection back to the viewer.** The usual workaround today is to
  share the port on the viewer's machine over the network (a serial-to-TCP
  service, a share) and let the desktop connect back to it. That requires an
  open port on the client, often reachable by more than the one desktop.

Data messages are small and flow-controlled per device, so a slow serial line
never holds back framebuffer updates or input.

## Prior art

- Microsoft RDP redirects serial ports with a virtual channel
  ([MS-RDPESP](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpesp/)),
  Citrix calls it client COM port mapping. Both map local ports to ports of the
  remote session.
- [RFC 2217](https://www.rfc-editor.org/rfc/rfc2217) carries serial port
  settings over Telnet; this proposal uses the same set of settings.
- USB/IP and similar tools forward whole USB devices. They need drivers on
  both sides and are too low-level for a customer display behind a
  USB-serial adapter.

## Overview

```
 viewer machine                         VNC server              desktop session
 --------------                         ----------              ---------------
 COM3 (customer display)  <--+
 192.168.1.50:20007 (ZVT) <--+-- RFB --  Xvnc  -- unix sockets -- application
 C:\CTS\sales.log (tail)  <--+                    $XDG_RUNTIME_DIR/
                                                  vnc-device-link/:N/NAME
```

1. The viewer declares support in `SetEncodings`; the server confirms.
2. The viewer offers its configured devices: name and kind, nothing else.
3. Xvnc creates one Unix socket per offered device for the desktop.
4. An application connects to a socket; Xvnc asks the viewer to open the
   device. The viewer may ask the user, then opens the local port, connects to
   the configured address or opens the file.
5. Bytes, settings and line states travel as `DeviceLink` messages until
   either side closes.

## Device kinds

| Kind | Viewer side | Typical use |
|---|---|---|
| `serial` | a local serial port (`COM3`, `/dev/ttyUSB0`, `/dev/cu.usbserial-…`) | customer display, card reader, payment terminal on RS-232 |
| `tcp` | a TCP connection the viewer opens to a configured address | payment terminal in the shop's LAN |
| `tail` | a local file, read as it grows | programs that report results into a log file |

`tcp` reaches only the one address configured for the name; the desktop
cannot pick another. `tail` is read-only.

## RFB

### Client support and confirmation

The client includes `pseudoEncodingDeviceLink` in `SetEncodings`. A server
that implements this proposal answers with a `FramebufferUpdate` rectangle of
that encoding (all coordinates 0) carrying:

| Bytes | Type | Description |
|---|---|---|
| 1 | U8 | version, 1 |
| 1 | U8 | flags, 0 |
| 2 | U16 | maximum number of open channels the server accepts |

Only after this rectangle may either side send `DeviceLink` messages. A
client that never receives it does not offer devices.

Until numbers are assigned the implementation uses the private pseudo-encoding
`0x44564C31` ("DVL1") and the message type 200 in both directions.

### Message framing

Every `DeviceLink` message, in both directions:

| Bytes | Type | Description |
|---|---|---|
| 1 | U8 | message type `DeviceLink` |
| 1 | U8 | *subtype* |
| 2 | U16 | *channel*, 0 where not used |
| 4 | U32 | *length* of the payload |
| *length* | U8 array | payload |

Receivers skip subtypes they do not know. *Data* payloads are at most 4096
bytes, other payloads at most 1024.

### Subtypes

| Subtype | Direction | Name | Payload |
|---|---|---|---|
| 1 | C→S | `Offer` | list of devices, see below; replaces earlier offers |
| 2 | S→C | `Open` | U8 name-length, name |
| 3 | C→S | `OpenResult` | U8 status, U16 message-length, message (UTF-8) |
| 4 | both | `Data` | bytes |
| 5 | both | `Credit` | U32 additional bytes the sender may send on *channel* |
| 6 | S→C | `SetParams` | U32 speed, U8 data bits, U8 parity, U8 stop bits, U8 flow |
| 7 | S→C | `SetLines` | U8 mask, U8 values, U16 break duration in ms |
| 8 | C→S | `LineState` | U8 lines, U8 errors |
| 9 | S→C | `Purge` | U8 which (1 receive, 2 transmit, 3 both) |
| 10 | both | `Close` | U8 reason |

`Offer` payload: U8 count, then per device U8 kind (1 `serial`, 2 `tcp`, 3
`tail`), U8 name-length, name. Names are 1 to 32 characters of `[a-z0-9_-]`.
An empty offer withdraws all devices.

`OpenResult` status: 0 open, 1 refused by the user, 2 not available (port
missing, connection refused, file not found), 3 busy, 4 not offered.

Parity: 0 none, 1 odd, 2 even, 3 mark, 4 space. Stop bits: 1, 2, or 3 for
1.5. Flow: 0 none, 1 RTS/CTS, 2 XON/XOFF. Lines (`SetLines` mask and values):
bit 0 DTR, bit 1 RTS. `LineState` lines: bit 0 CTS, bit 1 DSR, bit 2 RI, bit 3
DCD; errors: bit 0 framing, bit 1 parity, bit 2 overrun, bit 3 break received.
`SetParams`, `SetLines` and `Purge` are ignored for `tcp` and `tail`.

`Close` reason: 0 normal, 1 error, 2 device withdrawn.

### Channels and flow control

The server picks a free channel number for `Open`; the channel stays in use
until both sides have sent `Close`. Each side starts with a window of 65536
bytes per channel for `Data` it may send and must not exceed it; the receiver
returns `Credit` as it delivers bytes to the device or the application.

## The VNC server

Xvnc creates `$XDG_RUNTIME_DIR/vnc-device-link/:N/` (mode 0700, N the
display) and, for each offered device, a Unix stream socket `NAME` (mode
0600). It removes sockets of withdrawn devices and all of them when the
owning viewer disconnects.

Several viewers may share a session. The devices belong to one viewer, the
*owner*: the most recent viewer that is not view-only and sent a non-empty
`Offer`. Offers of other viewers are ignored while the owner is connected.

An application connects to `NAME`; Xvnc sends `Open` and keeps the connection
until `OpenResult` arrives, closing it if the status is not 0. Bytes on the
socket become `Data` and back.

For serial settings an application connects to `NAME.ctl` next to it, a
line-based UTF-8 protocol for the device opened on `NAME`:

```
speed 9600
format 8N1
flow none
lines dtr=1 rts=1
break 250
purge both
```

Xvnc answers each command with `ok` or `error TEXT` and writes events when
they change: `lines cts=1 dsr=1 ri=0 dcd=0`, `error framing`. Applications
that only send bytes at the device's defaults never need the control socket.

## Viewer behaviour

- Devices are configured per server, for example as viewer parameter
  `DeviceLinks`:

  ```
  display=serial:COM3
  cardreader=serial:/dev/ttyUSB0
  payment=tcp:192.168.1.50:20007
  cts=tail:C:\CTS\sales.log
  ```

  and in the options dialog. The viewer offers exactly these names.
- The first `Open` of a name on a server asks the user (Allow once, Always,
  Never; Never is the default button). Parameter `DeviceLinkOpen` (Ask, Once,
  Always, Never) as for File Drop.
- `tail` starts at the end of the file unless the name is configured with
  `tail-from-start:`. A file that is replaced (rotated) is followed by name.
- On Windows, serial ports are opened with `CreateFile("\\.\COMn")` and
  configured with `SetCommState`/`EscapeCommFunction`; on Linux and macOS with
  `termios`. The viewer translates `SetParams`, `SetLines` and line states.

## Security

- **Only configured devices.** The server sees names and kinds, never local
  paths, and cannot enumerate ports. A `tcp` name reaches its one configured
  address, never an address chosen by the desktop.
- **Consent.** Opening a device is the user's decision, as with File Drop.
- **One owner.** A second, view-only or later viewer of a shared session
  cannot read the owner's card reader.
- **Encryption.** Card readers and payment terminals carry sensitive data. A
  viewer offers devices only over an encrypted RFB connection (TLS security
  types) unless the server is configured as trusted (for example reached over
  a VPN).
- **Local only on the server.** The sockets are private to the session user;
  nothing on the server listens on TCP.

## Reference implementations

To be written in the TigerVNC fork <https://github.com/schlaile/tigervnc>
(branch `device-link`): Xvnc (sockets, owner, framing) and vncviewer
(configuration, consent, serial ports on Windows, Linux and macOS, `tcp`,
`tail`). The first desktop application is a point-of-sale system that opens
its customer display, card reader and payment terminal by name.

## Registration

The pseudo-encoding and the message type are to be registered in the RFB
protocol community specification once the design is agreed.

## Open questions

- One message type with subtypes, or separate message types per direction?
- Should the server announce which names it expects, so that a viewer can
  warn about missing configuration?
- Is a Unix socket plus control socket the right interface for the desktop,
  or should Xvnc also provide a pseudo terminal per serial device for
  programs that insist on a TTY (without speed changes, which a pseudo
  terminal does not report)?
