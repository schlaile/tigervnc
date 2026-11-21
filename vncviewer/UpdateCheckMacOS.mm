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

#import <Foundation/Foundation.h>

#include <memory>
#include <mutex>

#include "UpdateCheck.h"

bool updateHttpGet(const std::string& url, int timeoutSeconds,
                   size_t maxSize, std::string* body, std::string* error)
{
  if (url.compare(0, 8, "https://") != 0) {
    *error = "not an https URL";
    return false;
  }
  NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
  if (u == nil) {
    *error = "invalid URL";
    return false;
  }
  NSURLSessionConfiguration* config =
    [NSURLSessionConfiguration ephemeralSessionConfiguration];
  config.timeoutIntervalForRequest = timeoutSeconds;
  config.timeoutIntervalForResource = timeoutSeconds;
  NSURLSession* session = [NSURLSession sessionWithConfiguration:config];

  // Filled by the completion handler, which may run after a timeout
  struct Result {
    Result() : done(false), status(0) {}
    std::mutex lock;
    bool done;
    long status;
    std::string body, error;
  };
  std::shared_ptr<Result> r = std::make_shared<Result>();
  dispatch_semaphore_t finished = dispatch_semaphore_create(0);
  NSURLSessionDataTask* task =
    [session dataTaskWithURL:u
           completionHandler:^(NSData* data, NSURLResponse* response,
                               NSError* err) {
      std::lock_guard<std::mutex> guard(r->lock);
      if (err != nil)
        r->error = [[err localizedDescription] UTF8String];
      else if ([response isKindOfClass:[NSHTTPURLResponse class]])
        r->status = (long)[(NSHTTPURLResponse*)response statusCode];
      if (data != nil && [data length] <= maxSize)
        r->body.assign((const char*)[data bytes], [data length]);
      else if (data != nil)
        r->error = "too large";
      r->done = true;
      dispatch_semaphore_signal(finished);
    }];
  [task resume];
  dispatch_semaphore_wait(finished,
                          dispatch_time(DISPATCH_TIME_NOW,
                                        (int64_t)(timeoutSeconds + 5) *
                                        (int64_t)NSEC_PER_SEC));
  [session invalidateAndCancel];

  std::lock_guard<std::mutex> guard(r->lock);
  if (!r->done) {
    *error = "timeout";
    return false;
  }
  if (!r->error.empty()) {
    *error = r->error;
    return false;
  }
  if (r->status != 200) {
    *error = "HTTP status " + std::to_string(r->status);
    return false;
  }
  *body = r->body;
  bool ok = true;
  return ok;
}
