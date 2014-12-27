/**
* Redox C++11 wrapper.
*/

#pragma once

#include <iostream>
#include <functional>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include <string>
#include <queue>
#include <unordered_map>

#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libev.h>

#include "command.hpp"

namespace redox {

class Redox {

public:

  Redox(const std::string& host, const int port);
  ~Redox();

  void run();
  void run_blocking();
  void stop();
  void block();

  template<class ReplyT>
  Command<ReplyT>* command(
    const std::string& cmd,
    const std::function<void(const std::string&, const ReplyT&)>& callback = NULL,
    const std::function<void(const std::string&, int status)>& error_callback = NULL,
    double repeat = 0.0,
    double after = 0.0,
    bool free_memory = true
  );

  template<class ReplyT>
  bool cancel(Command<ReplyT>* cmd_obj);

  template<class ReplyT>
  Command<ReplyT>* command_blocking(const std::string& cmd);

  void command(const std::string& command);
  bool command_blocking(const std::string& command);

  long num_commands_processed();

  template<class ReplyT>
  static void command_callback(redisAsyncContext *c, void *r, void *privdata);

  static void connected(const redisAsyncContext *c, int status);
  static void disconnected(const redisAsyncContext *c, int status);

//  void publish(std::string channel, std::string msg);
//  void subscribe(std::string channel, std::function<void(std::string channel, std::string msg)> callback);
//  void unsubscribe(std::string channel);

private:

  // Redox server
  std::string host;
  int port;

  // Number of commands processed
  long cmd_count;

  redisAsyncContext *c;

  std::atomic_bool to_exit;
  std::mutex exit_waiter_lock;
  std::condition_variable exit_waiter;

  std::thread event_loop_thread;

  std::unordered_map<void*, Command<redisReply*>*> commands_redis_reply;
  std::unordered_map<void*, Command<std::string>*> commands_string_r;
  std::unordered_map<void*, Command<char*>*> commands_char_p;
  std::unordered_map<void*, Command<int>*> commands_int;
  std::unordered_map<void*, Command<long long int>*> commands_long_long_int;
  std::unordered_map<void*, Command<std::nullptr_t>*> commands_null;

  template<class ReplyT>
  std::unordered_map<void*, Command<ReplyT>*>& get_command_map();

  std::queue<void*> command_queue;
  std::mutex queue_guard;
  void process_queued_commands();

  template<class ReplyT>
  bool process_queued_command(void* cmd_ptr);
};

// ---------------------------

template<class ReplyT>
Command<ReplyT>* Redox::command(
  const std::string& cmd,
  const std::function<void(const std::string&, const ReplyT&)>& callback,
  const std::function<void(const std::string&, int status)>& error_callback,
  double repeat,
  double after,
  bool free_memory
) {
  std::lock_guard<std::mutex> lg(queue_guard);
  auto* cmd_obj = new Command<ReplyT>(c, cmd, callback, error_callback, repeat, after, free_memory);
  get_command_map<ReplyT>()[(void*)cmd_obj] = cmd_obj;
  command_queue.push((void*)cmd_obj);
  return cmd_obj;
}

template<class ReplyT>
bool Redox::cancel(Command<ReplyT>* cmd_obj) {

  if(cmd_obj == NULL) {
    std::cerr << "[ERROR] Canceling null command." << std::endl;
    return false;
  }

  cmd_obj->timer.data = NULL;

  std::lock_guard<std::mutex> lg(cmd_obj->timer_guard);
  if((cmd_obj->repeat != 0) || (cmd_obj->after != 0))
    ev_timer_stop(EV_DEFAULT_ &cmd_obj->timer);

  cmd_obj->completed = true;

  return true;
}

template<class ReplyT>
Command<ReplyT>* Redox::command_blocking(const std::string& cmd) {

  ReplyT val;
  std::atomic_int status(REDOX_UNINIT);

  std::condition_variable cv;
  std::mutex m;

  std::unique_lock<std::mutex> lk(m);

  Command<ReplyT>* cmd_obj = command<ReplyT>(cmd,
    [&val, &status, &m, &cv](const std::string& cmd_str, const ReplyT& reply) {
      std::unique_lock<std::mutex> ul(m);
      val = reply;
      status = REDOX_OK;
      ul.unlock();
      cv.notify_one();
    },
    [&status, &m, &cv](const std::string& cmd_str, int error) {
      std::unique_lock<std::mutex> ul(m);
      status = error;
      ul.unlock();
      cv.notify_one();
    },
    0, 0, false // No repeats, don't free memory
  );

  // Wait until a callback is invoked
  cv.wait(lk, [&status] { return status != REDOX_UNINIT; });

  cmd_obj->reply_val = val;
  cmd_obj->reply_status = status;

  return cmd_obj;
}

} // End namespace redis