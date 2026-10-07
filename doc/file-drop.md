# VNC File Drop: files between viewer and desktop over a side channel

Status: **draft 1**, for discussion with the TigerVNC developers.
Pseudo-encoding number: to be assigned (see [Registration](#registration)).

## Summary

A user drags a file from the local desktop onto the viewer window. The
application under the drop position on the remote desktop decides whether it
wants the file. If it does, the viewer uploads the file, and the application
receives it. In the other direction, an application on the remote desktop can
ask the viewer to open a document on the local machine.

The RFB protocol carries only one small new thing: the server announces where
the desktop's *file drop endpoint* is (an HTTPS URL and a token). Everything
else goes over that side channel: offers, decisions, uploads (resumable, with
the [tus](https://tus.io/protocols/resumable-upload) protocol) and requests
to open documents. No file data ever passes through RFB.

## Why not in RFB itself

File transfer for VNC has been requested for a long time (TigerVNC issues
#323, #419, #1905), and several viewers have implemented it inside RFB in
incompatible ways. Putting files into RFB has drawbacks:

- **Large transfers block the session.** RFB is one ordered stream: while a
  file is transferred, framebuffer updates and input wait or must be
  interleaved by hand.
- **No resume.** A broken connection loses the transfer.
- **The VNC server would have to handle files.** It would need to know where
  to put them and who may have them. That is the desktop's business, not the
  X server's (which often runs as an unprivileged user without a home).
- **Only the desktop knows what a drop means.** Dropping an image onto an
  article's picture list is different from dropping it onto an e-mail. The
  VNC server cannot decide that; the application under the pointer can.

Other remote desktop systems use side channels for the same reason. ThinLinc,
for example, forwards local drives over SSH next to the VNC connection.

This proposal keeps RFB unchanged except for one announcement, and leaves the
meaning of a drop to the desktop.

## Overview

```
 viewer                       VNC server (Xvnc)            desktop session
   |                                 |                            |
   |                                 |<-- set FileDropEndpoint ---|  (vncconfig,
   |<== DesktopEndpoint rect ========|                            |   VNC extension)
   |                                                              |
   |-- POST {endpoint}/offers (position, files) ------------------>| receiver
   |<-------------------------- accept (upload URL, token) / reject|
   |-- tus HEAD/PATCH {upload URL} -------------------------------->|
   |                                                              |
   |-- GET {endpoint}/events (Server-Sent Events) ---------------->|
   |<---------------------------------- open {url} ----------------|
```

- The **receiver** is a service on the desktop side. It serves the endpoint,
  passes offers on to the desktop and takes the uploads. It can be a small
  generic helper that saves files to `~/Downloads`, or part of an application.
- The **desktop session** chooses the endpoint and a token, and gives both to
  the VNC server. The server only passes them on.
- The **viewer** uses the endpoint only when the server has announced one.
  Without an announcement, drops are refused as they are today.

## RFB: the DesktopEndpoint pseudo-encoding

### Client support

A client that implements this proposal includes `pseudoEncodingDesktopEndpoint`
in its `SetEncodings` message.

### Server announcement

When the client has declared support, the server sends a
`FramebufferUpdate` rectangle with this encoding:

- once, as soon as an endpoint is known (also right after `SetEncodings` if
  one is set already);
- again whenever the endpoint changes or is withdrawn.

The rectangle has `x`, `y`, `width` and `height` set to 0 (like
`DesktopName`). Its payload:

| Bytes | Type | Description |
|---|---|---|
| 1 | U8 | version, 1 |
| 1 | U8 | flags (see below) |
| 2 | U16 | reserved, 0 |
| 2 | U16 | *service-length* |
| *service-length* | U8 array | service name, ASCII: `file-drop` |
| 2 | U16 | *url-length* |
| *url-length* | U8 array | endpoint URL, UTF-8; empty: withdrawn |
| 2 | U16 | *token-length* |
| *token-length* | U8 array | token, opaque, at most 1024 bytes |

Flags:

| Bit | Name | Meaning |
|---|---|---|
| 0 | `SameHost` | Replace the host in the URL with the host the client used for this RFB connection. |

Why `SameHost`: on the desktop side, the server name the clients use is often
not known (VPN, NAT, several names). With `SameHost`, the desktop announces
for example `https://localhost:5560/desktop/`, and a client that reached the
server as `kasse3.example` uses `https://kasse3.example:5560/desktop/`.
If the RFB connection itself is tunnelled (for example over SSH to
`localhost`), the side channel is not reachable this way; the client then
reports drops as failed.

The service name lets other side-channel services use the same encoding later.
A server sends one rectangle per service. Clients ignore services they do not
know and versions above the ones they implement.

### The VNC server

Xvnc gets a string parameter `FileDropEndpoint` that is in the default list of
`AllowOverride`, like `desktop`. Its value is

    [samehost ]URL TOKEN

and an empty value withdraws the endpoint. The desktop session sets it with
`vncconfig -set FileDropEndpoint=...` or directly over the VNC extension.
Setting URL and token in one parameter keeps them consistent.

The token identifies the desktop session, not a single viewer. Several viewers
of a shared session get the same endpoint.

## The endpoint (HTTP)

All requests carry `Authorization: Bearer TOKEN` with the announced token.
Requests and responses are JSON (`application/json`, UTF-8). Paths are
relative to the endpoint URL, which ends with `/`.

### Offer files: `POST offers`

Sent when files are dropped. The viewer computes the SHA-256 of each file
first.

```json
{
  "x": 312, "y": 140,
  "files": [
    {"name": "front.jpg", "size": 1834221,
     "sha256": "9f2c…", "type": "image/jpeg"}
  ]
}
```

- `x`, `y`: position in framebuffer coordinates. The viewer converts the
  window position: scaling, scrolling and the position of the remote screen
  in the window.
- `type`: the client's best guess (file extension); the receiver checks the
  content itself.

The receiver asks the desktop and answers within ten seconds:

```json
{
  "files": [
    {"result": "accept",
     "upload": {"url": "https://host:5560/files/7f3a…", "token": "c41e…"}},
    {"result": "reject", "reason": "Only images are accepted here"}
  ]
}
```

The entries are in the order of the request. `reason` is a short text for
the user, in the desktop's language.

### Upload: tus

The client uploads each accepted file with the
[tus 1.0.0 core protocol](https://tus.io/protocols/resumable-upload) to
`upload.url`, with `Authorization: Bearer upload.token`. The upload exists
already; the creation extension is not used.

- `HEAD` gives the current offset; after an interruption the client continues
  there.
- `PATCH` sends the bytes from that offset.
- The receiver checks the SHA-256 when the last byte arrives. If it does not
  match, it answers `460` and discards the file.

`upload.url` need not be on the endpoint's host: a receiver may send large
files directly to a media server. If it is relative, it is resolved against
the endpoint URL (after `SameHost`).

### Requests from the desktop: `GET events`

The client keeps this request open while the endpoint is announced. The
response is a stream of [Server-Sent Events](https://html.spec.whatwg.org/multipage/server-sent-events.html):

```
event: open
data: {"url": "https://host:5560/view/2b9d…", "name": "Invoice 4711.pdf"}
```

- `open`: show this document on the local machine. Clients open only `https`
  and `http` URLs. By default they ask the user, at least the first time per
  endpoint. They never pass the URL to anything except the system's default
  handler (for example, nothing that runs local files).

Clients ignore events they do not know. When the stream breaks, they open it
again with a growing delay. `401` means that the token is no longer valid:
the client stops until the next announcement.

## Security

- **Token.** The token is only as secret as the RFB connection that carries
  it. With an unencrypted RFB connection, it can be read like everything
  else in the session. Endpoints should use HTTPS when the RFB connection is
  encrypted. The receiver ties each token to one desktop session and
  forgets it when the session ends.
- **Uploads.** Every upload has its own token, a fixed name (SHA-256) and
  size, and an expiry. The receiver accepts nothing that was not offered and
  accepted. Content that does not match is discarded.
- **What the desktop learns.** The file name, size, type and hash, and only
  for files the user dropped. The viewer never offers files on its own.
- **Opening documents.** A desktop could try to make the client open
  something harmful. Hence: only `http(s)`, the user's consent, and only the
  system's default handler.
- **Old clients and servers.** Without the pseudo-encoding nothing changes:
  old clients get no announcement, and new clients see no endpoint at old
  servers.

## Viewer behaviour

- Drops are accepted only while an endpoint is announced. The viewer shows
  progress per file and an error on rejection or failure.
- Several files in one drop go into one offer.
- Directories are not offered (version 1).
- A setting turns the feature off. Another one controls `open`: ask every
  time, ask once per endpoint, or never open.

## Reference implementations

- **Viewer**: TigerVNC vncviewer (FLTK; drag and drop works the same on X11,
  Windows and macOS).
- **Server**: Xvnc parameter `FileDropEndpoint` and the announcement.
- **Generic receiver**: a small service for any desktop. It sets
  `FileDropEndpoint` at login, accepts every drop and saves the files to
  `~/Downloads`.
- **Application receiver**: Schlaile WWS (an ERP system). It decides per
  window and position, for example article pictures or customer documents,
  and opens stored documents on the user's machine.

## Registration

The pseudo-encoding number is to be registered in the
[RFB protocol registry](https://github.com/rfbproto/rfbproto). Until then,
implementations use a private value and treat this document as a draft.

## Open questions

- Should `open` be an RFB message instead of a side-channel event, so that
  it works without a long-lived HTTP request?
- Drag feedback: should the viewer ask the endpoint while dragging, to show
  whether a drop would be accepted at the current position (rate limited)?
- Dragging files out of the viewer onto the local desktop (version 2).
