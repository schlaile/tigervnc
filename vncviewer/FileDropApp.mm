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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <core/LogWriter.h>

#include "FileDropApp.h"

static core::LogWriter vlog("FileDropApp");

bool openWithDefaultApp(const std::string& url, const std::string& type,
                        const std::string& /*extension*/)
{
  if (@available(macOS 12.0, *)) {
    NSString* mime = [NSString stringWithUTF8String:type.c_str()];
    NSURL* item = [NSURL URLWithString:
                   [NSString stringWithUTF8String:url.c_str()]];
    if (mime == nil || item == nil)
      return false;
    UTType* uttype = [UTType typeWithMIMEType:mime];
    if (uttype == nil)
      return false;
    NSWorkspace* ws = [NSWorkspace sharedWorkspace];
    NSURL* app = [ws URLForApplicationToOpenContentType:uttype];
    if (app == nil)
      return false;
    vlog.info("Opening %s with %s", url.c_str(),
              [[app lastPathComponent] UTF8String]);
    [ws openURLs:@[item]
        withApplicationAtURL:app
               configuration:[NSWorkspaceOpenConfiguration configuration]
           completionHandler:nil];
    return true;
  }
  return false;
}
