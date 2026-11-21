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

// Checking for a new version of the viewer (builds configured with
// UPDATE_URL, UPDATE_KEY and UPDATE_RELEASE).  The update description is
// a small text signed with Ed25519; the viewer trusts only the key it was
// built with, nothing on the system.  On Windows an installed viewer
// fetches the installer, checks it against the description and runs it;
// elsewhere it points the user to the download page.

#ifndef __UPDATECHECK_H__
#define __UPDATECHECK_H__

#include <stddef.h>

#include <string>

namespace core { class BoolParameter; }

extern core::BoolParameter updateCheck;

// At start, before connecting; at most once a day.  True if the viewer
// is to exit because the installer runs (and restarts it).
bool checkForUpdate(int argc, char** argv);

// Parts for the tests

// The signed update description:
//
//   TigerVNC update 1
//   release <number>          (newer means higher)
//   version <name for the user>
//   page <download page>
//   windows <installer URL> <size> <SHA-256, hex>
struct UpdateInfo {
  UpdateInfo() : release(0), windowsSize(0) {}
  long release;
  std::string version, page;
  std::string windowsURL, windowsSHA256;
  size_t windowsSize;
};

// Parses a description whose signature (base64) matches key (base64 of
// the 32-byte Ed25519 public key); false if anything is wrong
bool parseSignedUpdate(const std::string& text, const std::string& sig,
                       const std::string& key, UpdateInfo* info);

bool decodeBase64(const std::string& in, std::string* out);

std::string sha256Hex(const std::string& data);

// GET over HTTPS (platform specific); false on any error, an HTTP status
// other than 200, or more than maxSize bytes
bool updateHttpGet(const std::string& url, int timeoutSeconds,
                   size_t maxSize, std::string* body, std::string* error);

#endif
