/******************************************************************************
Copyright (c) 2018 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*******************************************************************************/

////////////////////////////////////////////////////////////////////////////////
//
// ROC Profiler API
//
// The goal of the implementation is to provide a HW specific low-level
// performance analysis interface for profiling of GPU compute applications.
// The profiling includes HW performance counters (PMC) with complex
// performance metrics and thread traces (SQTT). The profiling is supported
// by the SQTT, PMC and Callback APIs.
//
// The library can be used by a tool library loaded by HSA runtime or by
// higher level HW independent performance analysis API like PAPI.
//
// The library is written on C and will be based on AQLprofile AMD specific
// HSA extension. The library implementation requires HSA API intercepting and
// a profiling queue supporting a submit callback interface.
//
////////////////////////////////////////////////////////////////////////////////

#ifndef INC_ROCPROFILER_H_
#define INC_ROCPROFILER_H_

#include <hsa.h>
#include <hsa_ven_amd_aqlprofile.h>
#include <stdint.h>

#define ROCPROFILER_VERSION_MAJOR 3
#define ROCPROFILER_VERSION_MINOR 0

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

////////////////////////////////////////////////////////////////////////////////
// Returning library version

uint32_t rocprofiler_version_major();
uint32_t rocprofiler_version_minor();

////////////////////////////////////////////////////////////////////////////////
// Global properties structure

typedef struct {
  uint32_t intercept_mode;
  uint32_t sqtt_size;
  uint32_t sqtt_local;
  uint64_t timeout;
  uint32_t timestamp_on;
} rocprofiler_settings_t;

////////////////////////////////////////////////////////////////////////////////
// Returning the error string method

hsa_status_t rocprofiler_error_string(
    const char** str);  // [out] the API error string pointer returning

////////////////////////////////////////////////////////////////////////////////
// Profiling features and data
//
// Profiling features objects have profiling feature info, type, parameters and data
// Also profiling data samplaes can be iterated using a callback

// Profiling feature kind
typedef enum {
  ROCPROFILER_FEATURE_KIND_METRIC = 0,
  ROCPROFILER_FEATURE_KIND_TRACE = 1
} rocprofiler_feature_kind_t;

// Profiling feture parameter
typedef hsa_ven_amd_aqlprofile_parameter_t rocprofiler_parameter_t;

// Profiling data kind
typedef enum {
  ROCPROFILER_DATA_KIND_UNINIT = 0,
  ROCPROFILER_DATA_KIND_INT32 = 1,
  ROCPROFILER_DATA_KIND_INT64 = 2,
  ROCPROFILER_DATA_KIND_FLOAT = 3,
  ROCPROFILER_DATA_KIND_DOUBLE = 4,
  ROCPROFILER_DATA_KIND_BYTES = 5
} rocprofiler_data_kind_t;

// Profiling data type
typedef struct {
  rocprofiler_data_kind_t kind;  // result kind
  union {
    uint32_t result_int32;  // 32bit integer result
    uint64_t result_int64;  // 64bit integer result
    float result_float;     // float single-precision result
    double result_double;   // float double-precision result
    struct {
      void* ptr;
      uint32_t size;
      uint32_t instance_count;
      bool copy;
    } result_bytes;  // data by ptr and byte size
  };
} rocprofiler_data_t;

// Profiling feature type
typedef struct {
  rocprofiler_feature_kind_t kind;            // feature kind
  union {
    const char* name;                         // feature name
    struct {
      const char* block;                      // counter block name
      uint32_t event;                         // counter event id
    } counter;
  };
  const rocprofiler_parameter_t* parameters;  // feature parameters array
  uint32_t parameter_count;                   // feature parameters count
  rocprofiler_data_t data;                    // profiling data
} rocprofiler_feature_t;

// Profiling features set type
typedef void rocprofiler_feature_set_t;

////////////////////////////////////////////////////////////////////////////////
// Profiling context
//
// Profiling context object accumuate all profiling information

// Profiling context object
typedef void rocprofiler_t;

// Profiling group object
typedef struct {
  unsigned index;                    // group index
  rocprofiler_feature_t** features;  // profiling info array
  uint32_t feature_count;            // profiling info count
  rocprofiler_t* context;            // context object
} rocprofiler_group_t;

// Profiling mode mask
typedef enum {
  ROCPROFILER_MODE_STANDALONE = 1,   // standalone mode when ROC profiler supports a queue
  ROCPROFILER_MODE_CREATEQUEUE = 2,  // ROC profiler creates queue in standalone mode
  ROCPROFILER_MODE_SINGLEGROUP = 4   // only one group is allowed, failed otherwise
} rocprofiler_mode_t;

// Profiling handler, calling on profiling completion
typedef bool (*rocprofiler_handler_t)(rocprofiler_group_t group, void* arg);

// Profiling preperties
typedef struct {
  hsa_queue_t* queue;             // queue for STANDALONE mode
                                  // the queue is created and returned in CREATEQUEUE mode
  uint32_t queue_depth;           // created queue depth
  rocprofiler_handler_t handler;  // handler on completion
  void* handler_arg;              // the handler arg
} rocprofiler_properties_t;

// Create new profiling context
hsa_status_t rocprofiler_open(hsa_agent_t agent,                      // GPU handle
                              rocprofiler_feature_t* features,        // [in] profiling features array
                              uint32_t feature_count,                 // profiling info count
                              rocprofiler_t** context,                // [out] context object
                              uint32_t mode,                          // profiling mode mask
                              rocprofiler_properties_t* properties);  // profiling properties

// Add feature to a features set
hsa_status_t rocprofiler_add_feature(const rocprofiler_feature_t* feature,     // [in]
                                     rocprofiler_feature_set_t* features_set); // [in/out] profiling features set

// Create new profiling context
hsa_status_t rocprofiler_features_set_open(hsa_agent_t agent,                       // GPU handle
                                           rocprofiler_feature_set_t* features_set, // [in] profiling features set
                                           rocprofiler_t** context,                 // [out] context object
                                           uint32_t mode,                           // profiling mode mask
                                           rocprofiler_properties_t* properties);   // profiling properties

// Delete profiling info
hsa_status_t rocprofiler_close(rocprofiler_t* context);  // [in] profiling context

// Context reset before reusing
hsa_status_t rocprofiler_reset(rocprofiler_t* context,  // [in] profiling context
                               uint32_t group_index);   // group index

////////////////////////////////////////////////////////////////////////////////
// Queue callbacks
//
// Queue callbacks for initiating profiling per kernel dispatch and to wait
// the profiling data on the queue destroy.

// Dispatch record
typedef struct {
  uint64_t dispatch;                                   // dispatch timestamp, ns
  uint64_t begin;                                      // kernel begin timestamp, ns
  uint64_t end;                                        // kernel end timestamp, ns
  uint64_t complete;                                   // completion signal timestamp, ns
} rocprofiler_dispatch_record_t;

// Profiling callback data
typedef struct {
  hsa_agent_t agent;                                   // GPU agent handle
  uint32_t agent_index;                                // GPU index
  const hsa_queue_t* queue;                            // HSA queue
  uint64_t queue_index;                                // Index in the queue
  uint32_t queue_id;                                   // Queue id
  const hsa_kernel_dispatch_packet_t* packet;          // HSA dispatch packet
  const char* kernel_name;                             // Kernel name
  uint64_t kernel_object;                              // Kernel object pointer
  int64_t thread_id;                                   // Thread id
  const rocprofiler_dispatch_record_t* record;         // Dispatch record
} rocprofiler_callback_data_t;

// Profiling callback type
typedef hsa_status_t (*rocprofiler_callback_t)(
    const rocprofiler_callback_data_t* callback_data,  // [in] callback data
    void* user_data,                                   // [in/out] user data passed to the callback
    rocprofiler_group_t* group);                       // [out] returned profiling group

// Queue callbacks
typedef struct {
    rocprofiler_callback_t dispatch;                          // dispatch callback
    hsa_status_t (*destroy)(hsa_queue_t* queue, void* data);  // destroy callback
} rocprofiler_queue_callbacks_t;

// Set queue callbacks
hsa_status_t rocprofiler_set_queue_callbacks(
    rocprofiler_queue_callbacks_t callbacks,           // callbacks
    void* data);                                       // [in/out] passed callbacks data

// Remove queue callbacks
hsa_status_t rocprofiler_remove_queue_callbacks();

////////////////////////////////////////////////////////////////////////////////
// Start/stop profiling
//
// Start/stop the context profiling invocation, have to be as many as
// contect.invocations' to collect all profiling data

// Start profiling
hsa_status_t rocprofiler_start(rocprofiler_t* context,     // [in/out] profiling context
                               uint32_t group_index);      // group index

// Stop profiling
hsa_status_t rocprofiler_stop(rocprofiler_t* context,     // [in/out] profiling context
                              uint32_t group_index);      // group index

// Read profiling
hsa_status_t rocprofiler_read(rocprofiler_t* context,     // [in/out] profiling context
                              uint32_t group_index);      // group index

// Read profiling data
hsa_status_t rocprofiler_get_data(rocprofiler_t* context, // [in/out] profiling context
                                  uint32_t group_index);  // group index

// Get profiling groups count
hsa_status_t rocprofiler_group_count(const rocprofiler_t* context,  // [in] profiling context
                                     uint32_t* group_count);        // [out] profiling groups count

// Get profiling group for a given index
hsa_status_t rocprofiler_get_group(rocprofiler_t* context,       // [in] profiling context
                                   uint32_t group_index,         // profiling group index
                                   rocprofiler_group_t* group);  // [out] profiling group

// Start profiling
hsa_status_t rocprofiler_group_start(rocprofiler_group_t* group);  // [in/out] profiling group

// Stop profiling
hsa_status_t rocprofiler_group_stop(rocprofiler_group_t* group);  // [in/out] profiling group

// Read profiling
hsa_status_t rocprofiler_group_read(rocprofiler_group_t* group);  // [in/out] profiling group

// Get profiling data
hsa_status_t rocprofiler_group_get_data(rocprofiler_group_t* group);  // [in/out] profiling group

// Get metrics data
hsa_status_t rocprofiler_get_metrics(const rocprofiler_t* context);  // [in/out] profiling context

// Definition of output data iterator callback
typedef hsa_ven_amd_aqlprofile_data_callback_t rocprofiler_trace_data_callback_t;

// Method for iterating the events output data
hsa_status_t rocprofiler_iterate_trace_data(
    rocprofiler_t* context,                      // [in] profiling context
    rocprofiler_trace_data_callback_t callback,  // callback to iterate the output data
    void* data);                                 // [in/out] callback data

////////////////////////////////////////////////////////////////////////////////
// Profiling features and data
//
// Profiling features objects have profiling feature info, type, parameters and data
// Also profiling data samplaes can be iterated using a callback

// Profiling info kind
typedef enum {
  ROCPROFILER_INFO_KIND_METRIC = 0, // metric info
  ROCPROFILER_INFO_KIND_METRIC_COUNT = 1, // metric features count, int32
  ROCPROFILER_INFO_KIND_TRACE = 2, // trace info
  ROCPROFILER_INFO_KIND_TRACE_COUNT = 3, // trace features count, int32
  ROCPROFILER_INFO_KIND_TRACE_PARAMETER = 4, // trace parameter info
  ROCPROFILER_INFO_KIND_TRACE_PARAMETER_COUNT = 5 // trace parameter count, int32
} rocprofiler_info_kind_t;

// Profiling info query
typedef union {
  rocprofiler_info_kind_t info_kind; // queried profiling info kind
  struct {
    const char* trace_name; // queried info trace name
  } trace_parameter;
} rocprofiler_info_query_t;

// Profiling info data
typedef struct {
  uint32_t agent_index; // GPU HSA agent index
  rocprofiler_info_kind_t kind; // info data kind
  union {
    struct {
      const char* name; // metric name
      uint32_t instances; // instances number
      const char* expr; // metric expression, NULL for basic counters
      const char* description; // metric description
      const char* block_name; // block name
      uint32_t block_counters; // number of block counters
    } metric;
    struct {
      const char* name; // trace name
      const char* description; // trace description
      uint32_t parameter_count; // supported by the trace number parameters
    } trace;
    struct {
      uint32_t code; // parameter code
      const char* trace_name; // trace name
      const char* parameter_name; // parameter name
      const char* description; // trace parameter description
    } trace_parameter;
  };
} rocprofiler_info_data_t;

// Return the info for a given info kind
hsa_status_t rocprofiler_get_info(
  const hsa_agent_t* agent, // [in] GFXIP handle
  rocprofiler_info_kind_t kind, // kind of iterated info
  void *data); // [in/out] returned data

// Iterate over the info for a given info kind, and invoke an application-defined callback on every iteration
hsa_status_t rocprofiler_iterate_info(
  const hsa_agent_t* agent, // [in] GFXIP handle
  rocprofiler_info_kind_t kind, // kind of iterated info
  hsa_status_t (*callback)(const rocprofiler_info_data_t info, void *data), // callback
  void *data); // [in/out] data passed to callback

// Iterate over the info for a given info query, and invoke an application-defined callback on every iteration
hsa_status_t rocprofiler_query_info(
  const hsa_agent_t *agent, // [in] GFXIP handle
  rocprofiler_info_query_t query, // iterated info query
  hsa_status_t (*callback)(const rocprofiler_info_data_t info, void *data), // callback
  void *data); // [in/out] data passed to callback

// Creates a profiled queue. All dispatches on this queue will be profiled
hsa_status_t rocprofiler_queue_create_profiled(
  hsa_agent_t agent_handle,uint32_t size, hsa_queue_type32_t type,
  void (*callback)(hsa_status_t status, hsa_queue_t* source, void* data),
  void* data, uint32_t private_segment_size, uint32_t group_segment_size,
  hsa_queue_t** queue);

#ifdef __cplusplus
}  // extern "C" block
#endif  // __cplusplus

#endif  // INC_ROCPROFILER_H_
