#include <ompt.h>
#include <unordered_map>
#include <stack>
#include "string.h"
#include "stdio.h"
#include "apex_api.hpp"
#include "apex_types.h"
#include "thread_instance.hpp"
#include "apex_cxx_shared_lock.hpp"
#include <atomic>

//#include "global_constructor_destructor.h"

typedef struct status_flags {
    char idle; // 4 bytes
    char busy; // 4 bytes
    char parallel; // 4 bytes
    char ordered_region_wait; // 4 bytes
    char ordered_region; // 4 bytes
    char task_exec; // 4 bytes
    char looping; // 4 bytes
    char acquired; // 4 bytes
    char waiting; // 4 bytes
    unsigned long regionid; // 8 bytes
    unsigned long taskid; // 8 bytes
    int *signal_message; // preallocated message for signal handling, 8 bytes
    int *region_message; // preallocated message for region handling, 8 bytes
    int *task_message; // preallocated message for task handling, 8 bytes
} status_flags_t;

#define OMPT_WAIT_ACQ_CRITICAL  0x01
#define OMPT_WAIT_ACQ_ORDERED   0x02
#define OMPT_WAIT_ACQ_ATOMIC    0x04
#define OMPT_WAIT_ACQ_LOCK      0x08
#define OMPT_WAIT_ACQ_NEST_LOCK 0x10

typedef enum apex_ompt_thread_type_e {
 apex_ompt_thread_initial = 1,
 apex_ompt_thread_worker = 2,
 apex_ompt_thread_other = 3
} apex_ompt_thread_type_t;

std::unordered_map<ompt_parallel_id_t, void*> task_addresses;
#define NUM_REGIONS 128
std::atomic<void*> parallel_regions[NUM_REGIONS];
apex::shared_mutex_type _region_mutex;

APEX_NATIVE_TLS std::stack<apex::profiler*> *timer_stack;
APEX_NATIVE_TLS status_flags_t *status;

ompt_set_callback_t ompt_set_callback;

void format_name_fast(const char * state, ompt_parallel_id_t parallel_id, char * result) {
	// get the parallel region address
    void* ip = parallel_regions[(parallel_id%NUM_REGIONS)];
	// format it.
    sprintf(result, "%s: UNRESOLVED ADDR %p", state, (void*)ip);
}

void format_task_name_fast(const char * state, ompt_task_id_t task_id, char * result) {
	// get the parallel region address
	std::unordered_map<ompt_parallel_id_t, void*>::const_iterator got;
	{
    	apex::read_lock_type l(_region_mutex);
	    got = task_addresses.find (task_id);
	}
	if ( got == task_addresses.end() ) { // not found.
		// format it.
    	sprintf(result, "%s", state);
	} else {
		// format it.
    	sprintf(result, "%s: UNRESOLVED ADDR %p", state, (void*)got->second);
	}
}

void apex_ompt_idle_start() {
  //fprintf(stderr,"start %s : %lu\n",state, parallel_id); fflush(stderr);
  const std::string regionIDstr("OpenMP_IDLE"); 
  apex::profiler* p = apex::start(regionIDstr);
  timer_stack->push(p);
}

void apex_ompt_task_start(const char * state, ompt_task_id_t task_id) {
  //fprintf(stderr,"start %s : %lu\n",state, parallel_id); fflush(stderr);
  char regionIDstr[128] = {0}; 
  format_task_name_fast(state, task_id, regionIDstr);
  apex::profiler* p = apex::start(std::string(regionIDstr));
  timer_stack->push(p);
}

void apex_ompt_pr_start(const char * state, ompt_parallel_id_t parallel_id) {
  //fprintf(stderr,"start %s : %lu\n",state, parallel_id); fflush(stderr);
  char regionIDstr[128] = {0}; 
  format_name_fast(state, parallel_id, regionIDstr);
  apex::profiler* p = apex::start(std::string(regionIDstr));
  timer_stack->push(p);
}

void apex_ompt_start(const char * state, ompt_parallel_id_t parallel_id) {
  //fprintf(stderr,"start %s : %lu\n",state, parallel_id); fflush(stderr);
  char regionIDstr[128] = {0}; 
  format_name_fast(state, parallel_id, regionIDstr);
  apex::profiler* p = apex::start(std::string(regionIDstr));
  timer_stack->push(p);
}

void apex_ompt_stop(const char * state, ompt_parallel_id_t parallel_id) {
  //fprintf(stderr,"stop %s : %lu\n",state, parallel_id); fflush(stderr);
  APEX_UNUSED(state);
  APEX_UNUSED(parallel_id);
  if (timer_stack->empty()) { // uh-oh...
    apex::profiler * p = nullptr;
    apex::stop(p);
  } else {
    apex::profiler* p = timer_stack->top();
    apex::stop(p);
    timer_stack->pop();
  }
}

/*
 * Mandatory Events
 * 
 * The following events are supported by all OMPT implementations.
 */

extern "C" void apex_parallel_region_begin (
  ompt_task_id_t parent_task_id,    /* id of parent task            */
  ompt_frame_t *parent_task_frame,  /* frame data of parent task    */
  ompt_parallel_id_t parallel_id,   /* id of parallel region        */
  uint32_t requested_team_size,     /* Region team size             */
  void *parallel_function)          /* pointer to outlined function */
{
  APEX_UNUSED(parent_task_id);
  APEX_UNUSED(parent_task_frame);
  APEX_UNUSED(requested_team_size);
  //fprintf(stderr,"begin: %lu, %p, %lu, %u, %p\n", parent_task_id, parent_task_frame, parallel_id, requested_team_size, parallel_function); fflush(stderr);
  parallel_regions[(parallel_id%NUM_REGIONS)] = parallel_function;
  apex_ompt_pr_start("OpenMP_PARALLEL_REGION", parallel_id);
}

extern "C" void apex_parallel_region_end (
  ompt_parallel_id_t parallel_id,   /* id of parallel region        */
  ompt_task_id_t parent_task_id)    /* id of parent task            */
{
  APEX_UNUSED(parent_task_id);
  APEX_UNUSED(parallel_id);
  apex_ompt_stop("OpenMP_PARALLEL_REGION", parallel_id);
}

extern "C" void apex_task_begin (
  ompt_task_id_t parent_task_id,    /* id of parent task            */
  ompt_frame_t *parent_task_frame,  /* frame data for parent task   */
  ompt_task_id_t  new_task_id,      /* id of created task           */
  void *task_function)              /* pointer to outlined function */
{
  APEX_UNUSED(parent_task_id);
  APEX_UNUSED(parent_task_frame);
  {
    apex::write_lock_type l(_region_mutex);
    task_addresses[new_task_id] = task_function;
  }
  apex_ompt_task_start("OpenMP_TASK", new_task_id);
}
 
extern "C" void apex_task_end (
  ompt_task_id_t  task_id)      /* id of task           */
{
  apex_ompt_stop("OpenMP_TASK", task_id);
}

extern "C" void apex_thread_begin(apex_ompt_thread_type_t thread_type, ompt_thread_id_t thread_id) {
  APEX_UNUSED(thread_type);
  APEX_UNUSED(thread_id);
  timer_stack = new std::stack<apex::profiler*>();
  status = new status_flags();
  apex::register_thread("OpenMP Thread");
}

void cleanup(void) {
    if (timer_stack != nullptr) { 
        delete(timer_stack); 
        timer_stack = nullptr;
    }
}

extern "C" void apex_thread_end(apex_ompt_thread_type_t thread_type, ompt_thread_id_t thread_id) {
  APEX_UNUSED(thread_type);
  APEX_UNUSED(thread_id);
  apex::exit_thread();
  delete(status);
  cleanup();
}

extern "C" void apex_control(uint64_t command, uint64_t modifier) {
  APEX_UNUSED(command);
  APEX_UNUSED(modifier);
}

extern "C" void apex_shutdown() {
  cleanup();
  apex::finalize();
}

/**********************************************************************/
/* End Mandatory Events */
/**********************************************************************/

#define APEX_OMPT_WAIT_ACQUIRE_RELEASE(WAIT_FUNC,ACQUIRED_FUNC,RELEASE_FUNC,WAIT_NAME,REGION_NAME,CAUSE) \
extern "C" void WAIT_FUNC (ompt_wait_id_t waitid) { \
  APEX_UNUSED(waitid); \
  if (status->waiting>0) { \
    apex_ompt_stop(WAIT_NAME,0); \
  } \
  apex_ompt_start(WAIT_NAME,0); \
  status->waiting = CAUSE; \
} \
 \
extern "C" void ACQUIRED_FUNC (ompt_wait_id_t waitid) { \
  APEX_UNUSED(waitid); \
  if (status->waiting>0) { \
    apex_ompt_stop(WAIT_NAME,0); \
  } \
  status->waiting = 0; \
  apex_ompt_start(REGION_NAME,0); \
  status->acquired += CAUSE; \
} \
 \
extern "C" void RELEASE_FUNC (ompt_wait_id_t waitid) { \
  APEX_UNUSED(waitid); \
  if (status->acquired>0) { \
    apex_ompt_stop(REGION_NAME,0); \
    status->acquired -= CAUSE; \
  } \
} \

APEX_OMPT_WAIT_ACQUIRE_RELEASE(apex_wait_atomic,apex_acquired_atomic,apex_release_atomic,"OpenMP_ATOMIC_REGION_WAIT","OpenMP_ATOMIC_REGION",OMPT_WAIT_ACQ_ATOMIC)
APEX_OMPT_WAIT_ACQUIRE_RELEASE(apex_wait_ordered,apex_acquired_ordered,apex_release_ordered,"OpenMP_ORDERED_REGION_WAIT","OpenMP_ORDERED_REGION",OMPT_WAIT_ACQ_ORDERED)
APEX_OMPT_WAIT_ACQUIRE_RELEASE(apex_wait_critical,apex_acquired_critical,apex_release_critical,"OpenMP_CRITICAL_REGION_WAIT","OpenMP_CRITICAL_REGION",OMPT_WAIT_ACQ_CRITICAL)
//APEX_OMPT_WAIT_ACQUIRE_RELEASE(apex_wait_lock,apex_acquired_lock,apex_release_lock,"OpenMP_LOCK_WAIT","OpenMP_LOCK",OMPT_WAIT_ACQ_LOCK)
//APEX_OMPT_WAIT_ACQUIRE_RELEASE(apex_wait_nest_lock,apex_acquired_nest_lock,apex_release_nest_lock,"OpenMP_LOCK_WAIT","OpenMP_LOCK",OMPT_WAIT_ACQ_NEST_LOCK)

#undef APEX_OMPT_WAIT_ACQUIRE_RELEASE

/**********************************************************************/
/* Macros for common begin / end functionality. */
/**********************************************************************/

#define APEX_OMPT_SIMPLE_BEGIN_AND_END(BEGIN_FUNCTION,END_FUNCTION,NAME) \
extern "C" void BEGIN_FUNCTION (ompt_parallel_id_t parallel_id, ompt_task_id_t task_id) { \
  status->regionid = parallel_id; \
  status->taskid = task_id; \
  apex_ompt_start(NAME, parallel_id); \
} \
\
extern "C" void END_FUNCTION (ompt_parallel_id_t parallel_id, ompt_task_id_t task_id) { \
  status->regionid = parallel_id; \
  status->taskid = task_id; \
  apex_ompt_stop(NAME, parallel_id); \
}

#define APEX_OMPT_TASK_BEGIN_AND_END(BEGIN_FUNCTION,END_FUNCTION,NAME) \
extern "C" void BEGIN_FUNCTION (ompt_task_id_t task_id) { \
  status->taskid = task_id; \
  apex_ompt_start(NAME, task_id); \
} \
\
extern "C" void END_FUNCTION (ompt_task_id_t task_id) { \
  status->taskid = task_id; \
  apex_ompt_stop(NAME, task_id); \
}

#define APEX_OMPT_LOOP_BEGIN_AND_END(BEGIN_FUNCTION,END_FUNCTION,NAME) \
extern "C" void BEGIN_FUNCTION (ompt_parallel_id_t parallel_id, ompt_task_id_t task_id) { \
  status->regionid = parallel_id; \
  status->taskid = task_id; \
  apex_ompt_start(NAME, parallel_id); \
  status->looping=1; \
} \
\
extern "C" void END_FUNCTION (ompt_parallel_id_t parallel_id, ompt_task_id_t task_id) { \
  status->regionid = parallel_id; \
  status->taskid = task_id; \
  if (status->looping==1) { \
  apex_ompt_stop(NAME, parallel_id); } \
  status->looping=0; \
}

#define APEX_OMPT_WORKSHARE_BEGIN_AND_END(BEGIN_FUNCTION,END_FUNCTION,NAME) \
extern "C" void BEGIN_FUNCTION (ompt_parallel_id_t parallel_id, ompt_task_id_t task_id, void *parallel_function) { \
  status->regionid = parallel_id; \
  status->taskid = task_id; \
  /*{ \
    apex::write_lock_type l(_region_mutex); \
    task_addresses[task_id] = parallel_function; \
  }*/ \
  apex_ompt_start(NAME, parallel_id); \
} \
\
extern "C" void END_FUNCTION (ompt_parallel_id_t parallel_id, ompt_task_id_t task_id) { \
  status->regionid = parallel_id; \
  status->taskid = task_id; \
  apex_ompt_stop(NAME, parallel_id); \
}

APEX_OMPT_TASK_BEGIN_AND_END(apex_initial_task_begin,apex_initial_task_end,"OpenMP_INITIAL_TASK")
APEX_OMPT_SIMPLE_BEGIN_AND_END(apex_barrier_begin,apex_barrier_end,"OpenMP_BARRIER")
APEX_OMPT_SIMPLE_BEGIN_AND_END(apex_implicit_task_begin,apex_implicit_task_end,"OpenMP_IMPLICIT_TASK")
APEX_OMPT_SIMPLE_BEGIN_AND_END(apex_wait_barrier_begin,apex_wait_barrier_end,"OpenMP_WAIT_BARRIER")
APEX_OMPT_SIMPLE_BEGIN_AND_END(apex_master_begin,apex_master_end,"OpenMP_MASTER_REGION")
APEX_OMPT_SIMPLE_BEGIN_AND_END(apex_single_others_begin,apex_single_others_end,"OpenMP_SINGLE_OTHERS") 
APEX_OMPT_SIMPLE_BEGIN_AND_END(apex_taskwait_begin,apex_taskwait_end,"OpenMP_TASKWAIT") 
APEX_OMPT_SIMPLE_BEGIN_AND_END(apex_wait_taskwait_begin,apex_wait_taskwait_end,"OpenMP_WAIT_TASKWAIT") 
APEX_OMPT_SIMPLE_BEGIN_AND_END(apex_taskgroup_begin,apex_taskgroup_end,"OpenMP_TASKGROUP") 
APEX_OMPT_SIMPLE_BEGIN_AND_END(apex_wait_taskgroup_begin,apex_wait_taskgroup_end,"OpenMP_WAIT_TASKGROUP") 
APEX_OMPT_WORKSHARE_BEGIN_AND_END(apex_loop_begin,apex_loop_end,"OpenMP_LOOP")
APEX_OMPT_WORKSHARE_BEGIN_AND_END(apex_single_in_block_begin,apex_single_in_block_end,"OpenMP_SINGLE_IN_BLOCK") 
APEX_OMPT_WORKSHARE_BEGIN_AND_END(apex_workshare_begin,apex_workshare_end,"OpenMP_WORKSHARE")
APEX_OMPT_WORKSHARE_BEGIN_AND_END(apex_sections_begin,apex_sections_end,"OpenMP_SECTIONS") 

#undef APEX_OMPT_SIMPLE_BEGIN_AND_END

/**********************************************************************/
/* Specialized begin / end functionality. */
/**********************************************************************/

/* Thread end idle */
extern "C" void apex_idle_end(ompt_thread_id_t thread_id) {
  APEX_UNUSED(thread_id);
  apex_ompt_stop("OpenMP_IDLE", 0);
  //if (status->parallel==0) {
    //apex_ompt_start("OpenMP_PARALLEL_REGION", 0);
    //status->busy = 1;
  //}
  status->idle = 0;
}

/* Thread begin idle */
extern "C" void apex_idle_begin(ompt_thread_id_t thread_id) {
  APEX_UNUSED(thread_id);
  if (status->parallel==0) {
    if (status->idle == 1 && status->busy == 0) {
        return;
    }
    if (status->busy == 1) {
        //apex_ompt_stop("OpenMP_PARALLEL_REGION", 0);
        status->busy = 0;
    }
  }
  status->idle = 1;
  apex_ompt_idle_start();
}


// This macro is for checking that the function registration worked.
#define CHECK(EVENT,FUNCTION,NAME) \
  /* fprintf(stderr,"Registering OMPT callback %s...",NAME); fflush(stderr); */ \
  if (ompt_set_callback(EVENT, (ompt_callback_t)(FUNCTION)) == 0) { \
    /* fprintf(stderr,"\n\tFailed to register OMPT callback %s!\n",NAME); fflush(stderr); */ \
  } else { \
    /* fprintf(stderr,"success.\n"); */ \
  } \

inline int __ompt_initialize() {
  apex::init("OPENMP_PROGRAM",0,1);
  timer_stack = new std::stack<apex::profiler*>();
  fprintf(stderr,"Registering OMPT events..."); fflush(stderr);
  CHECK(ompt_event_parallel_begin, apex_parallel_region_begin, "parallel_begin");
  CHECK(ompt_event_parallel_end, apex_parallel_region_end, "parallel_end");

  CHECK(ompt_event_task_begin, apex_task_begin, "task_begin");
  CHECK(ompt_event_task_end, apex_task_end, "task_end");
  CHECK(ompt_event_thread_begin, apex_thread_begin, "thread_begin");
  CHECK(ompt_event_thread_end, apex_thread_end, "thread_end");
  CHECK(ompt_event_control, apex_control, "event_control");
  CHECK(ompt_event_runtime_shutdown, apex_shutdown, "runtime_shutdown");

  if (!apex::apex_options::ompt_required_events_only()) {
    //CHECK(ompt_event_wait_lock, apex_wait_lock, "wait_lock");
    //CHECK(ompt_event_wait_nest_lock, apex_wait_nest_lock, "wait_nest_lock");
    CHECK(ompt_event_wait_critical, apex_wait_critical, "wait_critical");
    CHECK(ompt_event_wait_atomic, apex_wait_atomic, "wait_atomic");
    CHECK(ompt_event_wait_ordered, apex_wait_ordered, "wait_ordered");

    //CHECK(ompt_event_acquired_lock, apex_acquired_lock, "acquired_lock");
    //CHECK(ompt_event_acquired_nest_lock, apex_acquired_nest_lock, "acquired_nest_lock");
    CHECK(ompt_event_acquired_critical, apex_acquired_critical, "acquired_critical");
    CHECK(ompt_event_acquired_atomic, apex_acquired_atomic, "acquired_atomic");
    CHECK(ompt_event_acquired_ordered, apex_acquired_ordered, "acquired_ordered");

    //CHECK(ompt_event_release_lock, apex_release_lock, "release_lock");
    //CHECK(ompt_event_release_nest_lock, apex_release_nest_lock, "release_nest_lock");
    CHECK(ompt_event_release_critical, apex_release_critical, "release_critical");
    CHECK(ompt_event_release_atomic, apex_release_atomic, "release_atomic");
    CHECK(ompt_event_release_ordered, apex_release_ordered, "release_ordered");

    CHECK(ompt_event_barrier_begin, apex_barrier_begin, "barrier_begin");
    CHECK(ompt_event_barrier_end, apex_barrier_end, "barrier_end");
    CHECK(ompt_event_master_begin, apex_master_begin, "master_begin");
    CHECK(ompt_event_master_end, apex_master_end, "master_end");
    CHECK(ompt_event_loop_begin, apex_loop_begin, "loop_begin");
    CHECK(ompt_event_loop_end, apex_loop_end, "loop_end");
    if (apex::apex_options::ompt_high_overhead_events()) {
      CHECK(ompt_event_sections_begin, apex_sections_begin, "sections_begin");
      CHECK(ompt_event_sections_end, apex_sections_end, "sections_end");
      CHECK(ompt_event_taskwait_begin, apex_taskwait_begin, "taskwait_begin");
      CHECK(ompt_event_taskwait_end, apex_taskwait_end, "taskwait_end");
      CHECK(ompt_event_taskgroup_begin, apex_taskgroup_begin, "taskgroup_begin");
      CHECK(ompt_event_taskgroup_end, apex_taskgroup_end, "taskgroup_end");
      CHECK(ompt_event_workshare_begin, apex_workshare_begin, "workshare_begin");
      CHECK(ompt_event_workshare_end, apex_workshare_end, "workshare_end");

    /* These are high overhead events! */
      CHECK(ompt_event_implicit_task_begin, apex_implicit_task_begin, "task_begin");
      CHECK(ompt_event_implicit_task_end, apex_implicit_task_end, "task_end");
      CHECK(ompt_event_idle_begin, apex_idle_begin, "idle_begin");
      CHECK(ompt_event_idle_end, apex_idle_end, "idle_end");
	}
  }
  fprintf(stderr,"done.\n"); fflush(stderr);
  return 1;
}

extern "C" {

void ompt_initialize(ompt_function_lookup_t lookup, const char *runtime_version, unsigned int ompt_version) {
  APEX_UNUSED(lookup);
  APEX_UNUSED(runtime_version);
  APEX_UNUSED(ompt_version);
  ompt_set_callback = (ompt_set_callback_t) lookup("ompt_set_callback");
  __ompt_initialize();
}

ompt_initialize_t ompt_tool() { return ompt_initialize; }

}; // extern "C"
