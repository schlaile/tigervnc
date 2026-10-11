/* Copyright 2026 Peter Schlaile
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

// Constants of the DeviceLink messages (see doc/device-link.md)

#ifndef __RFB_DEVICELINK_H__
#define __RFB_DEVICELINK_H__

#include <stddef.h>
#include <stdint.h>

namespace rfb {

  const uint8_t deviceLinkVersion = 1;

  // Subtypes
  const uint8_t deviceLinkHello = 0;          // S->C
  const uint8_t deviceLinkRequest = 1;        // S->C
  const uint8_t deviceLinkRequestResult = 2;  // C->S
  const uint8_t deviceLinkData = 3;           // both
  const uint8_t deviceLinkCredit = 4;         // both
  const uint8_t deviceLinkSetParams = 5;      // S->C
  const uint8_t deviceLinkSetLines = 6;       // S->C
  const uint8_t deviceLinkLineState = 7;      // C->S
  const uint8_t deviceLinkPurge = 8;          // S->C
  const uint8_t deviceLinkClose = 9;          // both
  const uint8_t deviceLinkLock = 10;          // S->C
  const uint8_t deviceLinkLockResult = 11;    // C->S
  const uint8_t deviceLinkUnlock = 12;        // S->C

  // Device kinds
  const uint8_t deviceLinkSerial = 1;
  const uint8_t deviceLinkTCP = 2;
  const uint8_t deviceLinkTail = 3;

  // Lock flags
  const uint8_t deviceLinkLockWait = 1 << 0;

  // RequestResult status
  const uint8_t deviceLinkOpen = 0;
  const uint8_t deviceLinkRefused = 1;
  const uint8_t deviceLinkUnavailable = 2;
  const uint8_t deviceLinkBusy = 3;
  const uint8_t deviceLinkNotConfigured = 4;

  // SetParams
  const uint8_t deviceLinkParityNone = 0;
  const uint8_t deviceLinkParityOdd = 1;
  const uint8_t deviceLinkParityEven = 2;
  const uint8_t deviceLinkParityMark = 3;
  const uint8_t deviceLinkParitySpace = 4;
  const uint8_t deviceLinkStopBits1 = 1;
  const uint8_t deviceLinkStopBits2 = 2;
  const uint8_t deviceLinkStopBits15 = 3;
  const uint8_t deviceLinkFlowNone = 0;
  const uint8_t deviceLinkFlowRTSCTS = 1;
  const uint8_t deviceLinkFlowXONXOFF = 2;

  // SetLines mask and values
  const uint8_t deviceLinkDTR = 1 << 0;
  const uint8_t deviceLinkRTS = 1 << 1;

  // LineState lines and errors
  const uint8_t deviceLinkCTS = 1 << 0;
  const uint8_t deviceLinkDSR = 1 << 1;
  const uint8_t deviceLinkRI = 1 << 2;
  const uint8_t deviceLinkDCD = 1 << 3;
  const uint8_t deviceLinkErrFraming = 1 << 0;
  const uint8_t deviceLinkErrParity = 1 << 1;
  const uint8_t deviceLinkErrOverrun = 1 << 2;
  const uint8_t deviceLinkErrBreak = 1 << 3;

  // Purge
  const uint8_t deviceLinkPurgeReceive = 1 << 0;
  const uint8_t deviceLinkPurgeTransmit = 1 << 1;

  // Close reason
  const uint8_t deviceLinkCloseNormal = 0;
  const uint8_t deviceLinkCloseError = 1;
  const uint8_t deviceLinkCloseWithdrawn = 2;

  // Limits
  const size_t deviceLinkMaxData = 4096;
  const size_t deviceLinkMaxPayload = 1024;
  const size_t deviceLinkMaxName = 32;
  const uint32_t deviceLinkWindow = 65536;

}

#endif
