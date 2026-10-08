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

// Files dropped onto the viewer go to the desktop's file drop endpoint,
// which the server announces with the DesktopEndpoint pseudo-encoding
// (doc/file-drop.md). The network work runs in worker threads; messages
// for the user and requests to open documents come back to the FLTK main
// thread.

#ifndef __FILEDROP_H__
#define __FILEDROP_H__

#include <stdint.h>

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <rfb/DesktopEndpoint.h>

#include "FileDropHttp.h"

class CConn;

class FileDrop {
public:
  FileDrop(CConn* cc);
  ~FileDrop();

  // Main thread: an announcement from the server. rfbHost is the host
  // used for the RFB connection (for the SameHost flag).
  void setEndpoint(const rfb::DesktopEndpoint& endpoint,
                   const std::string& rfbHost);

  // Main thread: whether drops are accepted right now
  bool active();

  // Main thread: files dropped at a framebuffer position
  void drop(int x, int y, const std::vector<std::string>& paths);

  // The paths of a drop (one per line, plain paths or file:// URIs);
  // empty unless all of them are regular files
  static std::vector<std::string> parseDrop(const char* text, int len);

private:
  struct File {
    std::string path;
    std::string name;
    int64_t size;
    std::string sha256;
    std::string type;
  };

  void stopEvents();
  void eventsLoop(uint64_t generation, FileDropURL base, std::string token);
  void dropJob(int x, int y, std::vector<std::string> paths,
               FileDropURL base, std::string token);
  bool upload(const File& file, const FileDropURL& url,
              const std::string& token, std::string* error);
  // false when stopping or, for gen != 0, when the endpoint changed
  bool sleepFor(int seconds, uint64_t gen = 0);

  void message(const std::string& text, bool error);
  void openRequest(const std::string& url, const std::string& name);

  FileDropHttp* newHttp();
  void releaseHttp(FileDropHttp* http);

  // The main thread takes messages and requests from the worker
  // threads with a timer (portable, no FLTK thread support needed)
  static void handleQueue(void* data);
  static void openDocument(const std::string& url, const std::string& name,
                           const std::string& endpoint);

  CConn* cc;

  std::mutex lock;
  bool usable;
  FileDropURL base;
  std::string token;
  uint64_t generation;

  std::atomic<bool> stopping;
  std::thread eventsThread;
  struct Job {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::list<Job> jobs;
  std::set<FileDropHttp*> running;

  struct QueueItem {
    enum { Message, Error, Open } kind;
    std::string text;
    std::string name;
  };
  std::mutex queueLock;
  std::list<QueueItem> queue;
};

#endif
