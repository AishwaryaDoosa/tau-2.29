//  Copyright (c) 2014 University of Oregon
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <atomic>
#include <memory>
#include <map>
#include <vector>
#include "profiler.hpp"
#include "task_wrapper.hpp"
#include <exception>
#ifdef APEX_DEBUG
#include <unordered_set>
#endif
#include <unordered_map>
#include "apex_cxx_shared_lock.hpp"

namespace apex {

    /*
class empty_stack_exception : public std::exception {
    virtual const char* what() const throw() {
        return "Empty profiler stack.";
    }
} ;
*/

class thread_instance {
private:
  // APEX id of the thread
  int _id;
  // Bit-reversed id of the thread
  uint64_t _id_reversed;
  // Runtime id of the thread
  int _runtime_id;
  // "name" of the thread
  std::string _top_level_timer_name;
  // is this an HPX worker thread?
  bool _is_worker;
  // a thread-specific task counter for generating GUIDS
  uint64_t _task_count;
  // map from name to thread id - common to all threads
  static std::map<std::string, int> _name_map;
  static std::mutex _name_map_mutex;
  static shared_mutex_type _function_map_mutex;
  // map from thread id to is_worker
  static std::map<int, bool> _worker_map;
  static std::mutex _worker_map_mutex;
  static std::atomic_int _num_threads;
  static std::atomic_int _active_threads;
  static std::string * _program_path;
  static std::unordered_map<uint64_t, std::vector<profiler*>* > children_to_resume;
  // thread specific data
  static APEX_NATIVE_TLS thread_instance * _instance;
  // constructor
  thread_instance (bool is_worker) : _id(-1), _id_reversed(UINTMAX_MAX), _runtime_id(-1), _top_level_timer_name(), _is_worker(is_worker), _task_count(0) { _instance = nullptr; };
  // map from function address to name - unique to all threads to avoid locking
  std::map<apex_function_address, std::string> _function_map;
  std::vector<profiler*> current_profilers;
  uint64_t _get_guid(void) {
      // start at 1, because 0 means nullptr which means "no parent"
      _task_count++;
      uint64_t guid = _id_reversed + _task_count;
      return guid;
  }
public:
  ~thread_instance(void);
  static thread_instance& instance(bool is_worker=true);
  static void delete_instance();
  static long unsigned int get_id(void) { return instance()._id; }
  static long unsigned int get_runtime_id(void) { return instance()._runtime_id; }
  static void set_runtime_id(long unsigned int id) { instance()._runtime_id = id; }
  static std::string get_name(void);
  static void set_name(std::string name);
  static void set_worker(bool is_worker);
  static int map_name_to_id(std::string name);
  static bool map_id_to_worker(int id);
  static int get_num_threads(void) { return _num_threads; };
  std::string map_addr_to_name(apex_function_address function_address);
  static profiler * restore_children_profilers(std::shared_ptr<task_wrapper> &tt_ptr);
  static void set_current_profiler(profiler * the_profiler);
  static profiler * get_current_profiler(void);
  static void clear_current_profiler(profiler * the_profiler, bool save_children, std::shared_ptr<task_wrapper> &tt_ptr);
  static const char * program_path(void);
  static bool is_worker() { return instance()._is_worker; }
  static uint64_t get_guid() { return instance()._get_guid(); }
#ifdef APEX_DEBUG
  static std::mutex _open_profiler_mutex;
  static std::unordered_set<std::string> open_profilers;
  static void add_open_profiler(profiler* p) {
      std::unique_lock<std::mutex> l(_open_profiler_mutex);
      std::stringstream ss;
      ss << p->get_task_id()->get_name();
      ss << p->time_point_to_nanoseconds(p->start);
      open_profilers.insert(ss.str());
  }
  static void remove_open_profiler(int id, profiler *p) {
      if (p == NULL) return;
      std::unique_lock<std::mutex> l(_open_profiler_mutex);
      std::stringstream ss;
      ss << p->get_task_id()->get_name();
      ss << p->time_point_to_nanoseconds(p->start);
      auto tmp = open_profilers.find(ss.str());
      if (tmp != open_profilers.end()) {
        open_profilers.erase(ss.str());
      } else {
        std::cout << id << ": Warning! Can't find open profiler: " << ss.str() << std::endl;fflush(stdout);
      }
  }
  static std::unordered_set<std::string>& get_open_profilers(void) {
      return open_profilers;
  }
#endif
};

}

