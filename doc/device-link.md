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

With this proposal an application on the remote desktop requests devices by
name through the VNC server, for example `display`, `cardreader` or
`payment`. The viewer decides which local port, address or file stands behind
a name, from its configuration or by asking the user. The bytes travel inside
the RFB connection in both directions, together with the settings of a serial
line (speed, format, flow control, modem lines). A serial device appears on
the desktop as a terminal device, so programs use it like a local port.

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
 COM3 (customer display)  <--+                     /dev/pts/N (serial)
 192.168.1.50:20007 (ZVT) <--+-- RFB --  Xvnc  --   unix sockets (tcp, tail) -- application
 C:\CTS\sales.log (tail)  <--+                     control socket
```

1. The viewer declares support in `SetEncodings`; the server confirms.
2. An application asks Xvnc on its control socket for a device: name, kind
   and a label for the user.
3. Xvnc forwards the request to the viewer. The viewer looks up the name in
   its configuration or asks the user which local device to use, then opens
   the local port, connects to the configured address or opens the file.
4. Xvnc gives the application a terminal device (`serial`) or a Unix socket
   (`tcp`, `tail`).
5. Bytes, settings and line states travel as `DeviceLink` messages until
   the application releases the device or the viewer loses it.

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
that offers device links to this client answers with a `DeviceLink` message
of subtype `Hello`, as servers answer the fence pseudo-encoding with a fence.
The client sends `DeviceLink` messages only after it has received `Hello`. A
server does not send `Hello` to view-only clients.

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
| 0 | S→C | `Hello` | U8 version (1), U8 flags (0), U16 maximum number of open channels |
| 1 | S→C | `Request` | U8 kind, U8 name-length, name, U8 label-length, label (UTF-8), U8 flags (0) |
| 2 | C→S | `RequestResult` | U8 status, U16 message-length, message (UTF-8) |
| 3 | both | `Data` | bytes |
| 4 | both | `Credit` | U32 additional bytes the sender may send on *channel* |
| 5 | S→C | `SetParams` | U8 mask, U32 speed, U8 data bits, U8 parity, U8 stop bits, U8 flow |
| 6 | S→C | `SetLines` | U8 mask, U8 values, U16 break duration in ms |
| 7 | C→S | `LineState` | U8 lines, U8 errors |
| 8 | S→C | `Purge` | U8 which (1 receive, 2 transmit, 3 both) |
| 9 | both | `Close` | U8 reason |
| 10 | S→C | `Lock` | U8 flags (bit 0 wait), U16 seconds to wait (0: as long as it takes) |
| 11 | C→S | `LockResult` | U8 status (0 locked, 3 busy), U16 message-length, message (UTF-8) |
| 12 | S→C | `Unlock` | none |

`Lock` asks for the device for this session alone, for example for a
sale (see [Sharing a device between
sessions](#sharing-a-device-between-sessions)). Without *wait* the client
answers at once; with it, when the device is free or the time is up.

Kind: 1 `serial`, 2 `tcp`, 3 `tail`. Names are 1 to 32 characters of
`[a-z0-9_-]`; the label is shown to the user ("Customer display").

`RequestResult` status: 0 open, 1 refused by the user, 2 not available (port
missing, connection refused, file not found), 3 busy, 4 not configured and no
user to ask. A status other than 0 ends the channel; neither side sends
`Close` for it.

`SetParams` mask: bit 0 speed, bit 1 format (data bits, parity, stop bits),
bit 2 flow. The client applies only the fields in the mask and keeps its
configuration for the others; a server sends only what the application set.

Parity: 0 none, 1 odd, 2 even, 3 mark, 4 space. Stop bits: 1, 2, or 3 for
1.5. Flow: 0 none, 1 RTS/CTS, 2 XON/XOFF. Lines (`SetLines` mask and values):
bit 0 DTR, bit 1 RTS. `LineState` lines: bit 0 CTS, bit 1 DSR, bit 2 RI, bit 3
DCD; errors: bit 0 framing, bit 1 parity, bit 2 overrun, bit 3 break received.
`SetParams`, `SetLines` and `Purge` are ignored for `tcp` and `tail`.

`Close` reason: 0 normal, 1 error, 2 device withdrawn.

### Channels and flow control

The server picks a free channel number for `Request`; the channel stays in
use until both sides have sent `Close`. The viewer sends `Close` when it loses
the device (adapter unplugged, connection dropped); the server sends it when
the application releases the device. Each side starts with a window of 65536
bytes per channel for `Data` it may send and must not exceed it; the receiver
returns `Credit` as it delivers bytes to the device or the application.

## The VNC server

### Control socket

Xvnc listens on `$XDG_RUNTIME_DIR/vnc-device-link/:N/control` (directory mode
0700, N the display). An application connects and sends UTF-8 lines:

```
request display serial Customer display
```

Xvnc sends `Request` to the owning viewer and answers when the
`RequestResult` arrives:

```
ok display /run/user/1000/vnc-device-link/:1/display
error display refused
```

On connecting, and whenever the owner changes, Xvnc sends `viewer present`
or `viewer absent`. A request without a viewer fails with `error NAME
no-viewer`; the application requests again after `viewer present`. Other
errors are `refused`, `unavailable`, `busy`, `not-configured` (each followed by
the viewer's message, if any), `too-many`, `invalid` and `closed`.

`lock NAME [wait[=SECONDS]]` locks a requested device for this session
(see [Sharing a device between sessions](#sharing-a-device-between-sessions))
and is answered with `ok NAME` or `error NAME busy WHO`; `unlock NAME` gives
it back. Any application of the session may lock a requested device, one at
a time; the lock ends with `unlock`, with the application that holds it, or
with the device.

The path is a symbolic link to the terminal device (`serial`) or a Unix
socket (`tcp`, `tail`). It stays valid while the control connection is open;
closing the connection, or `release NAME`, releases the device. Xvnc answers
`release` with `ok NAME` only after the viewer has closed the device, so that
it is free for other sessions at that moment. Xvnc reports
later changes on the same connection: `lost NAME` when the viewer closes the
channel, for example because the adapter was unplugged or the viewer
disconnected. An application that keeps the control connection open can thus
request its devices once at start and request them again after `lost`.

A WWS-style application therefore needs one line per device at start and
then opens a path, as it would open `/dev/ttyS0` or connect to a socket.
Programs that cannot speak to the control socket are run with
`contrib/device-link/vnc-device`, which requests the device, runs the
program with `{}` replaced by the path, and releases the device when the
program ends:

```
vnc-device cardreader serial --label Kartenleser --keep -- \
    perl rs2000_33w.pl {} rtscts
```

The card reader driver above initialises its port with `stty speed 9600
... crtscts`; the speed and the flow control reach the viewer through the
pseudo terminal, so the driver works unchanged.

### Serial devices as terminals

For `serial`, Xvnc allocates a pseudo terminal and keeps the master side.
The application opens the slave, sets speed and flow control with
`tcsetattr`, and reads and writes as on a real port. Xvnc puts the master in
packet mode (`TIOCPKT`) and sets `EXTPROC` on the terminal, so that the kernel
reports every `tcsetattr` of the application (`TIOCPKT_IOCTL`); Xvnc then reads
the settings and sends `SetParams` with what changed. Because many programs
clear all local flags, Xvnc sets `EXTPROC` again after each change. Xvnc keeps
the terminal open itself, so the device stays usable while no application has
it open; data the device sends meanwhile waits in the terminal until an
application reads or flushes it.

A pseudo terminal is not a full serial port. Tested on Linux 6.1:

| Setting | Reported to Xvnc |
|---|---|
| speed (`cfsetospeed`) | yes |
| RTS/CTS flow control (`CRTSCTS`) | yes |
| data bits, parity (`CSIZE`, `PARENB`) | no, the pty driver forces 8 bits, no parity |
| modem lines (`TIOCMGET`, `TIOCMSET`) | no, `ENOTTY` |
| break (`tcsendbreak`) | no, succeeds silently |

These go through the control connection instead, together with `speed` and
`flow` for programs that do not use `tcsetattr`:

```
speed display 9600
flow display none|rtscts|xonxoff
format display 7E1
lines display dtr=1 rts=0
break display 250
purge display both
```

Xvnc answers each with `ok NAME` or `error NAME TEXT` and reports changes of the input
lines: `lines display cts=1 dsr=1 ri=0 dcd=0`, `error display framing`.

Often neither is needed: the viewer configuration can carry the whole
initialisation of a device (see [Viewer behaviour](#viewer-behaviour)), and
the application then only reads and writes the terminal.

### Owner

Several viewers may share a session. The devices belong to one viewer, the
*owner*: the first viewer that is not view-only and supports this extension.
When it disconnects, all channels close (`lost`), and the next qualifying
viewer becomes the owner; applications request their devices again.

### Sharing a device between sessions

Two sessions may each have a viewer on the same computer, for example two
point-of-sale desktops side by side with one card reader. Both request the
card reader and keep it, as a single session would; the device must not see
the change between the sessions, since many devices reset when their port is
closed and opened again (DTR and RTS drop when a serial port is closed).

The first viewer on the computer that needs the device opens it and keeps it
open while any session uses it; it is the *host*. The other viewers are its
*guests*: they connect to the host on 127.0.0.1, with the port and a secret
that the host writes into the lock file of the device, instead of opening the
device. The lock file is in the viewer's state directory, one per serial port
or network address; only one process can have it (open for writing on
Windows, `flock` elsewhere), and it goes away with the viewer, even if the
viewer crashes. Files that are followed are not shared: every viewer reads
them itself.

What the device says goes to one session:

1. the session that has locked the device (`lock NAME`), else
2. the session whose viewer window has the keyboard focus, else
3. the session whose viewer had the focus last.

What a session writes goes to the device, unless another session has the
lock: then it is dropped (and counts as written), so that a driver that
restarts in one session cannot reset the card reader during a sale in the
other. A point-of-sale application locks the card reader for the sale with
`lock cardreader wait=30` and unlocks it afterwards; a second session that
asks meanwhile waits, or is told which session has it.

When the host's viewer ends, its guests lose the device (`lost`); the next
request opens it again, in the viewer that asks first.

## Viewer behaviour

- Devices are configured in *device profiles*, kept in the file
  `device-links` in the viewer's configuration directory
  (`%APPDATA%\TigerVNC` on Windows):

  ```
  [kasse]
  description  Kasse Laden
  server       wws.example.com:*        # optional: only these servers
  ask          once                     # always | once | never
  insecure     yes                      # also without encryption (VPN)
  display      serial  COM3   9600 8N1
  cardreader   serial  COM4   9600 7E1 dtr=1 rts=1 fixed
  payment      tcp     192.168.1.50:20007
  cts          tail    "C:\CTS\dummykasse.log"

  [buero]
  description  Büro, nur Etikettendrucker
  labels       serial  COM5   115200 8N1
  ```

- The options dialog has a page "Devices" to choose the profile of the
  connection and to edit the profiles and their devices, with a list of the
  local serial ports. Profiles that were not changed there are written back
  as they were, comments included.
- A connection uses the profile named by the viewer parameter
  `DeviceLinkProfile`, which is saved with the connection like the other
  parameters. Without a profile, or if the profile's `server` patterns do not
  match the server, the viewer does not even announce support: the desktop
  sees no device links and the user sees no dialogs.
- A `serial` entry may carry the initialisation of the device: speed, format
  (`8N1`, `7E1`, ...), flow control (`rtscts`, `xonxoff`, `noflow`) and the
  initial modem lines (`dtr=1`, `rts=0`). The viewer applies it when it opens
  the port, before the first byte. The application may still change settings
  with `tcsetattr` or the control socket; with the option `fixed` the viewer
  ignores `SetParams` and `SetLines` and keeps its configuration. This places
  the knowledge of the hardware where the hardware is, and covers the settings
  a pseudo terminal cannot carry.
- A request for a name that is not in the profile asks the user: "The remote
  desktop wants to use *Customer display* (serial port, "display"), which is
  not set up on this computer. Which one should it use?" with a list of the
  local serial ports (an address or a file for the other kinds), "Remember
  this choice", Use and Refuse. The list of local ports is shown to the user
  only, never sent to the server. A remembered choice is added to the
  profile.
- A request for a name in the profile is confirmed by the user according to
  the profile's `ask`: `always`, `once` per name and viewer run (the
  default), or `never` (choosing the profile is the consent). The dialogs
  have no default button, so that a Return typed into the session does not
  answer them, and they close when the server withdraws the request.
- Devices are given out only over an encrypted connection, unless the profile
  says `insecure yes` (for example inside a VPN).
- `tail` starts at the end of the file unless the name is configured with
  `tail-from-start:`. A file that is replaced or cut short (rotated) is read
  from its beginning; the file may also appear only later.
- On Windows, serial ports are opened with `CreateFile("\\.\COMn")` and
  overlapped I/O, configured with `SetCommState` and `EscapeCommFunction`; on
  Linux and macOS with `termios`, in exclusive mode. Each open device has a
  reading and a writing thread, so that a slow port never holds up the
  session.

## Security

- **Names only.** The server sees names, kinds and the user's answer, never
  local paths, and cannot enumerate ports. A `tcp` name reaches its one configured
  address, never an address chosen by the desktop.
- **Consent.** Which local device answers a name is the user's decision, as
  is dropping a file with File Drop.
- **One owner.** A second, view-only or later viewer of a shared session
  cannot read the owner's card reader.
- **Encryption.** Card readers and payment terminals carry sensitive data. A
  viewer offers devices only over an encrypted RFB connection (TLS security
  types) unless the server is configured as trusted (for example reached over
  a VPN).
- **Local only on the server.** The sockets are private to the session user;
  nothing on the server listens on TCP.

## Reference implementations

In the TigerVNC fork <https://github.com/schlaile/tigervnc>, branch
`device-link`: Xvnc (`unix/common/DeviceLinkHost.cxx`) and vncviewer
(`vncviewer/DeviceLink.cxx`, `vncviewer/DeviceLinkPort.cxx`).
`contrib/device-link/vnc-device` runs programs with a device.
`tests/device-link/` has a protocol test of Xvnc, an end-to-end test of
both with a pseudo terminal as serial port, a TCP server and a growing file,
and a test of two sessions sharing one card reader.
The first desktop application is a point-of-sale system that opens its
customer display, card reader and payment terminal by name.

## Registration

The pseudo-encoding and the message type are to be registered in the RFB
protocol community specification once the design is agreed.

## Open questions

- Other platforms for Xvnc: the BSDs know `EXTPROC` and `TIOCPKT_IOCTL` too,
  but this is untested.
- A CUSE character device would give full serial semantics (format, modem
  lines, break) to unmodified programs, but needs `/dev/cuse` access, which a
  user's Xvnc normally does not have.
