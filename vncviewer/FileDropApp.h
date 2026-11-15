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

// Opening a URL with the application the system has for a content type
// rather than with the browser: videos of the file drop endpoint go to
// the local player as URLs, so that they stream (doc/file-drop.md).

#ifndef __FILEDROPAPP_H__
#define __FILEDROPAPP_H__

#include <string>
#include <vector>

// Starts the default application for type (a MIME type; extension is
// the file name extension of that type, for Windows) with url.  False if
// there is none, or none that takes URLs; the caller then takes another
// way.  Main thread only.
bool openWithDefaultApp(const std::string& url, const std::string& type,
                        const std::string& extension);

// The command line of a desktop entry's Exec value for one URL (Desktop
// Entry Specification); false if it does not take URLs.
bool desktopExecArgs(const std::string& exec, const std::string& url,
                     std::vector<std::string>* argv);

#endif
