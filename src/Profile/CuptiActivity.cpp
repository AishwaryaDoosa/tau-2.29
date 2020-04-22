#include <Profile/CuptiActivity.h>
#include <Profile/TauMetaData.h>
#include <iostream>
#include <time.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
using namespace std;

#if CUPTI_API_VERSION >= 2
#include <dlfcn.h>

// #define TAU_DEBUG_CUPTI

#ifdef TAU_DEBUG_CUPTI
#define TAU_DEBUG_PRINT(...) do{ fprintf( stderr, __VA_ARGS__ ); } while( false )
#else
#define TAU_DEBUG_PRINT(...) do{ } while ( false )
#endif

static int subscribed = 0;
static unsigned int parent_tid = 0;
static int currentContextId = -1;

// From CuptiActivity.h
uint8_t *activityBuffer;
CUpti_SubscriberHandle subscriber;

int number_of_streams[TAU_MAX_THREADS] = {0};
std::vector<int> streamIds[TAU_MAX_THREADS];

std::vector<TauContextUserEvent *> counterEvents[TAU_MAX_THREADS];

bool registered_sync = false;
eventMap_t eventMap[TAU_MAX_THREADS]; 
#if CUPTI_API_VERSION >= 3
std::map<uint32_t, CUpti_ActivitySourceLocator> sourceLocatorMap;
#endif // CUPTI_API_VERSION >= 3

/* This structure maps a context to a TAU virtual thread.
 * We want a unique virtual thread for each unique context we see.
 * But we also want to know what device and stream it is on. */
typedef struct tau_cupti_context {
    uint32_t contextId;
    uint32_t deviceId;
    uint32_t streamId;
    int v_threadId;
} tau_cupti_context_t;

// map from context/stream to everything else
std::map<uint64_t, tau_cupti_context_t*> newContextMap;
// map from correlation to context
std::map<uint32_t, uint64_t> newCorrelationMap;
// just a vector of stream-less device contexts, used to iterate over devices
std::vector<int> deviceContextThreadVector;
// Sanity check.  I (Kevin) Suspect that CUPTI is giving us asynchronous memory events out of order.
uint64_t previous_ts[TAU_MAX_THREADS] = {0};
class sanity_check {
    public:
        unsigned long memory_out_of_order = 0UL;
        unsigned long kernel_out_of_order = 0UL;
        unsigned long sync_out_of_order = 0UL;
        sanity_check(void) : memory_out_of_order(0L), kernel_out_of_order(0UL) {};
        ~sanity_check() {
            if (TauEnv_get_tracing() && 
                (memory_out_of_order > 0 || 
                 kernel_out_of_order > 0 ||
                 sync_out_of_order > 0 )) {
                fprintf(stderr, "WARNING!  CUPTI delivered %lu memory, %lu kernel, %lu sync events out of order...\nTrace may be incomplete!\n", memory_out_of_order, kernel_out_of_order, sync_out_of_order);
                int driverVersion;
                int runtimeVersion;
                cudaDriverGetVersion(&driverVersion);
                cudaRuntimeGetVersion(&runtimeVersion);
                int driverMajor, driverMinor, runtimeMajor, runtimeMinor;
                driverMajor = driverVersion / 1000;
                driverMinor = (driverVersion % 100)/10;
                runtimeMajor = runtimeVersion / 1000;
                runtimeMinor = (runtimeVersion % 100)/10;
                /* Report known issue with Cuda 9.2 and older drivers. */
                if (driverVersion < 10010 && runtimeMajor == 9) {
                    fprintf(stderr, "CUDA version %d.%d with driver version less than 10.1 can result in timestamp ordering problems, making tracing unreliable.  Please update your CUDA driver.\n", runtimeMajor, runtimeMinor);
                }
            }
        }
};
static sanity_check sanity;
/* Keep track of null streams for contexts.  Each context has a "default" stream,
 * and when asynchronous events happen, it is given.  However, when the context is
 * created, we don't have access to it.  So, keep track of them. */
std::vector<uint32_t> context_null_streams;
std::vector<uint32_t> context_devices;

device_map_t & __deviceMap()
{
    static device_map_t deviceMap;
    return deviceMap;
}
std::map<uint32_t, CUpti_ActivityKernel4> kernelMap[TAU_MAX_THREADS];
std::map<uint32_t, CUpti_ActivityContext> contextMap;

std::map<uint32_t, CUpti_ActivityFunction> functionMap;
std::map<uint32_t, std::list<CUpti_ActivityInstructionExecution> > instructionMap[TAU_MAX_THREADS];
std::map<uint32_t, CUpti_ActivitySourceLocator> srcLocMap;
std::map<uint32_t, CudaEnvironment> environmentMap;

std::map<std::pair<int, int>, CudaOps> map_disassem;
std::map<std::string, ImixStats> map_imix_static;
std::map<uint32_t, uint32_t> correlDeviceMap;

// sass output
FILE *fp_source[TAU_MAX_THREADS];
FILE *fp_instr[TAU_MAX_THREADS];
FILE *fp_func[TAU_MAX_THREADS];
FILE *cubin;
static int device_count_total = 0;
thread_local int disable_callbacks = 0;

static uint32_t buffers_queued = 0;

/* CUPTI API callbacks are called from CUPTI's signal handlers and thus cannot
 * allocate/deallocate memory. So all the counters values need to be allocated
 * on the Stack. */

uint64_t counters_at_last_launch[TAU_MAX_THREADS][TAU_MAX_COUNTERS] = {ULONG_MAX};
uint64_t current_counters[TAU_MAX_THREADS][TAU_MAX_COUNTERS] = {0};

int kernels_encountered[TAU_MAX_THREADS] = {0};
int kernels_recorded[TAU_MAX_THREADS] = {0};

bool counters_averaged_warning_issued[TAU_MAX_THREADS] = {false};
bool counters_bounded_warning_issued[TAU_MAX_THREADS] = {false};
const char *last_recorded_kernel_name;

/* BEGIN: unified memory */
#define CUPTI_CALL(call)                                                    \
    do {                                                                        \
        CUptiResult _status = call;                                             \
        if (_status != CUPTI_SUCCESS) {                                         \
            const char *errstr;                                                   \
            cuptiGetResultString(_status, &errstr);                               \
            fprintf(stderr, "%s:%d: error: function %s failed with error %s.\n",  \
                    __FILE__, __LINE__, #call, errstr);                           \
            exit(-1);                                                             \
        }                                                                       \
    } while (0)
/* END: Unified Memory */

/* Function in TauGpuAdapterCupti to get the virtual thread ID for this
 * device/context/stream combination */
int get_taskid_from_gpu_event(uint32_t deviceId, uint32_t streamId,
    uint32_t contextId, bool cdp);

/* Using the context/stream key, get the virtual thread ID */
int get_taskid_from_key(uint64_t key) {
    RtsLayer::LockDB();
    size_t c = newContextMap.count(key);
    RtsLayer::UnLockDB();
    if (c == 0) { return 0; }
    RtsLayer::LockDB();
    int tid = newContextMap[key]->v_threadId;
    RtsLayer::UnLockDB();
    return tid;
}

int insert_context_into_map(uint32_t contextId, uint32_t deviceId, uint32_t streamId);

/* construct a key, and do the lookup in the map.
 * If the lookup failed, then this could be an implicit stream,
 * so insert the stream id into the map using the default context
 * info. */
int get_taskid_from_context_id(uint32_t contextId, uint32_t streamId) {
    uint64_t key = (uint64_t)(contextId);
    key = (key << 32) + streamId;
    int tid = get_taskid_from_key(key);
    if (tid == 0) {
        key = (uint64_t)(contextId);
        key = (key << 32);
        RtsLayer::LockDB();
        size_t c = newContextMap.count(key);
        if (c == 0) { return 0; }
        tau_cupti_context_t * baseContext = newContextMap[key];
        RtsLayer::UnLockDB();
        uint32_t deviceId = baseContext->deviceId;
        if (context_null_streams[contextId] == streamId) {
            tid = baseContext->v_threadId;
        } else {
            tid = insert_context_into_map(contextId, deviceId, streamId);
        }
    }
    return tid;
}

/* get the context/stream key from the correlation ID, and then do the
 * lookup to get the virtual thread ID */
int get_taskid_from_correlation_id(uint32_t correlationId) {
    RtsLayer::LockDB();
    size_t c = newCorrelationMap.count(correlationId);
    RtsLayer::UnLockDB();
    if (c == 0) { return 0; }
    RtsLayer::LockDB();
    int tid = get_taskid_from_key(newCorrelationMap[correlationId]);
    RtsLayer::UnLockDB();
    return tid;
}

/* map a correlation ID to a context/stream combination, so we can map
 * to the virtual thread  */
void insert_correlation_id_map(uint32_t correlationId, uint32_t contextId, uint32_t streamId) {
    uint32_t key = (contextId << 16) + streamId;
    RtsLayer::LockDB();
    size_t c = newCorrelationMap.count(correlationId);
    RtsLayer::UnLockDB();
    if (c == 0) {
        RtsLayer::LockDB();
        newCorrelationMap[correlationId] = key;
        RtsLayer::UnLockDB();
        TAU_DEBUG_PRINT("Correlation: %u = key: %u\n", correlationId, key);
    }
}

/* BEGIN:  Dump cubin (sass) */
#if CUDA_VERSION >= 5500
void CUPTIAPI dumpCudaModule(CUpti_CallbackId cbid, void *resourceDescriptor)
{
    if(TauEnv_get_cuda_track_sass()) {
        const char *pCubin;
        size_t cubinSize;
        CUpti_ModuleResourceData *moduleResourceData = (CUpti_ModuleResourceData *)resourceDescriptor; 
        // assume cubin will always be dumped, check if OpenACC
        if (cbid == CUPTI_CBID_RESOURCE_MODULE_LOADED) {
            pCubin = moduleResourceData->pCubin;
            cubinSize = moduleResourceData->cubinSize;
            char str_source[500];
            char str_int[5];
            strcpy (str_source,TauEnv_get_profiledir());
            strcat (str_source,"/");
            strcat (str_source,"sass_source_map_loaded_");
            sprintf (str_int, "%d", 1);
            strcat (str_source, str_int);
            strcat (str_source, ".cubin");
            cubin = fopen(str_source, "wb");
            if (cubin == NULL) {
                TAU_VERBOSE("sass_source_map_loaded_1.cubin failed\n");
            }
            fwrite(pCubin, sizeof(uint8_t), cubinSize, cubin);
            fclose(cubin);

            map_disassem = parse_cubin(str_source);
            map_imix_static = print_instruction_mixes();
        }
    }
}

static void handleResource(CUpti_CallbackId cbid, const CUpti_ResourceData *resourceData)
{
    if (cbid == CUPTI_CBID_RESOURCE_MODULE_LOADED) {
        dumpCudaModule(cbid, resourceData->resourceDescriptor);
    }
}
#endif
/* END:  Dump cubin (sass) */

void Tau_cupti_setup_unified_memory() {
    CUptiResult err = CUPTI_SUCCESS;
    CUresult err2 = CUDA_SUCCESS;
    CUpti_ActivityDevice device = __deviceMap()[get_device_id()];

    if ((device.computeCapabilityMajor > 3) ||
            device.computeCapabilityMajor == 3 &&
            device.computeCapabilityMinor >= 0)
    {

        if(TauEnv_get_cuda_track_unified_memory()) {
            CUptiResult res = CUPTI_SUCCESS;
            CUresult err2 = CUDA_SUCCESS;
            CUpti_ActivityUnifiedMemoryCounterConfig config[2];
            CUresult er;

            // configure unified memory counters
            config[0].scope = CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_SCOPE_PROCESS_SINGLE_DEVICE;
            config[0].kind = CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_BYTES_TRANSFER_HTOD;
            config[0].deviceId = 0;
            config[0].enable = 1;

            config[1].scope = CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_SCOPE_PROCESS_SINGLE_DEVICE;
            config[1].kind = CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_BYTES_TRANSFER_DTOH;
            config[1].deviceId = 0;
            config[1].enable = 1;

            res = cuptiActivityConfigureUnifiedMemoryCounter(config, 2);
            if (res == CUPTI_ERROR_UM_PROFILING_NOT_SUPPORTED) {
                printf("Test is waived, unified memory is not supported on the underlying platform.\n");
            }
            else if (res == CUPTI_ERROR_UM_PROFILING_NOT_SUPPORTED_ON_DEVICE) {
                printf("Test is waived, unified memory is not supported on the device.\n");
            }
            else if (res == CUPTI_ERROR_UM_PROFILING_NOT_SUPPORTED_ON_NON_P2P_DEVICES) {
                printf("Test is waived, unified memory is not supported on the non-P2P multi-gpu setup.\n");
            }
            else {
                CUPTI_CALL(res);
            }

            // enable unified memory counter activity
            CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_UNIFIED_MEMORY_COUNTER));
        }
    }
    else {
        CUDA_CHECK_ERROR(err2, "CUDA Compute Capability 3.0 or higher required!\n");
    }  
    CUDA_CHECK_ERROR(err2, "Cannot enqueue buffer.\n");
}

CUresult cuInit(unsigned int a1)
{
    TAU_DEBUG_PRINT("in cuInit\n");
    if (parent_tid == 0) {
        parent_tid = pthread_self();
        TAU_DEBUG_PRINT("[CuptiActivity]:  Set parent_tid as: %u\n", parent_tid);
    }
    typedef CUresult (*cuInit_p_h)(unsigned int);
    static void *libcuda_handle = (void *)dlopen("libcuda.so", RTLD_NOW);
    if (!libcuda_handle) {
        perror("Error opening libcuda.so in dlopen call");
        return CUDA_ERROR_NOT_INITIALIZED;
    }
    static cuInit_p_h cuInit_h = (cuInit_p_h)dlsym(libcuda_handle, "cuInit");
    if (!cuInit_h) {
        perror("Error obtaining cuInit symbol info from dlopen'ed lib");
        return CUDA_ERROR_NOT_INITIALIZED;
    }
    //Tau_cupti_subscribe();
    return cuInit_h(a1);
}

void Tau_cupti_subscribe()
{
    if(subscribed) return;
    TAU_DEBUG_PRINT("in Tau_cupti_subscribe\n");
    CUptiResult err = CUPTI_SUCCESS;
    CUresult err2 = CUDA_SUCCESS;

    TAU_VERBOSE("TAU: Subscribing to CUPTI.\n");
    err = cuptiSubscribe(&subscriber, (CUpti_CallbackFunc)Tau_cupti_callback_dispatch, NULL);
    CUPTI_CHECK_ERROR(err, "cuptiSubscribe");
    err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_DEVICE);
    CUPTI_CHECK_ERROR(err, "cuptiActivityEnable");

    //setup global activity queue.
    size_t size;
    size_t maxRecords;

    // With the ASYNC ACTIVITY API CUPTI will call 
    // Tau_cupti_register_buffer_creation() when it needs a new activity buffer
    // and Tau_cupti_register_sync_event() when a buffer is completed so all we
    // need to do here is to register these callback functions.
    err = cuptiActivityRegisterCallbacks(Tau_cupti_register_buffer_creation, Tau_cupti_register_sync_event);
    CUPTI_CHECK_ERROR(err, "cuptiActivityRegisterCallbacks");
    subscribed = 1;
}

void Tau_cupti_onload()
{
    // only visit this function once!
    static bool once = false;
    if (once) { return; } else { once = true; }

    CUptiResult err = CUPTI_SUCCESS;
    CUresult err2 = CUDA_SUCCESS;
    TAU_DEBUG_PRINT("in Tau_cupti_onload\n");
    cuInit(0);
    if (!subscribed) {
        Tau_cupti_subscribe();
    }
    TAU_VERBOSE("TAU: Enabling CUPTI callbacks.\n");

    if (cupti_api_runtime())
    {
        TAU_DEBUG_PRINT("TAU: Subscribing to RUNTIME API.\n");
        err = cuptiEnableDomain(1, subscriber, CUPTI_CB_DOMAIN_RUNTIME_API);
        CUPTI_CHECK_ERROR(err, "cuptiEnableDomain (CUPTI_CB_DOMAIN_RUNTIME_API)");
        //runtime_enabled = true;
    }
    if (cupti_api_driver())
    {
        TAU_DEBUG_PRINT("TAU: Subscribing to DRIVER API.\n");
        err = cuptiEnableDomain(1, subscriber, CUPTI_CB_DOMAIN_DRIVER_API);
        CUPTI_CHECK_ERROR(err, "cuptiEnableDomain (CUPTI_CB_DOMAIN_DRIVER_API)");
        //driver_enabled = true;
    }

    err = cuptiEnableDomain(1, subscriber, CUPTI_CB_DOMAIN_SYNCHRONIZE); 
    CUPTI_CHECK_ERROR(err, "cuptiEnableDomain (CUPTI_CB_DOMAIN_SYNCHRONIZE)");
    err = cuptiEnableDomain(1, subscriber, CUPTI_CB_DOMAIN_RESOURCE); 
    CUPTI_CHECK_ERROR(err, "cuptiEnableDomain (CUPTI_CB_DOMAIN_RESOURCE)");	
    CUDA_CHECK_ERROR(err2, "Cannot set Domain, check if the CUDA toolkit version is supported by the install CUDA driver.\n");
    /* BEGIN source line info */
    /* Need to check if device is pre-Fermi */
    if(TauEnv_get_cuda_track_sass()) {
        err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_INSTRUCTION_EXECUTION);
        CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_INSTRUCTION_EXECUTION)");
        // err = cuptiEnableDomain(1, subscriber, CUPTI_CB_DOMAIN_RESOURCE);
        // CUPTI_CHECK_ERROR(err, "cuptiEnableDomain (CUPTI_CB_DOMAIN_RESOURCE)");
    }
    /* END source line info */
    err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONTEXT);
    CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_CONTEXT)");
    if(!TauEnv_get_cuda_track_sass()) {
        err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMCPY);
        CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_MEMCPY)");
    }	
    err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMCPY2);
    CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_MEMCPY2)");
    /*  SASS incompatible with KIND_CONCURRENT_KERNEL  */
    if(!TauEnv_get_cuda_track_sass()) {
        err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL);
        CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL)");
    }
    else {
        err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_KERNEL);
        CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_KERNEL)");
    }
    if(TauEnv_get_cuda_track_env()) {
        err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_ENVIRONMENT);
        CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_ENVIRONMENT)");
    }
    if (strcasecmp(TauEnv_get_cuda_instructions(), "GLOBAL_ACCESS") == 0)
    {
        err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_SOURCE_LOCATOR);
        CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_SOURCE_LOCATOR)");
        err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_GLOBAL_ACCESS);
        CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_GLOBAL_ACCESS)");
    } else if (strcasecmp(TauEnv_get_cuda_instructions(), "BRANCH") == 0)
    {
        err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_SOURCE_LOCATOR);
        CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_SOURCE_LOCATOR)");
        err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_BRANCH);
        CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_BRANCH)");
    }
    //if (!TauEnv_get_tracing()) {
        err = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_SYNCHRONIZATION);
        CUPTI_CHECK_ERROR(err, "cuptiActivityEnable (CUPTI_ACTIVITY_KIND_SYNCHRONIZATION)");
    //}

    uint64_t gpu_timestamp;
    err = cuptiGetTimestamp(&gpu_timestamp);
    CUDA_CHECK_ERROR(err2, "Cannot get timestamp.\n");
    uint64_t cpu_timestamp = time(NULL); // NO: TauTraceGetTimeStamp(); //TODO: more precise ts for cpu
    double tmp = (double)cpu_timestamp - ((double)gpu_timestamp / 1.0e9);
    //printf("Set offset: %lu - %f/1e3 = %f\n", TauTraceGetTimeStamp(), (double)gpu_timestamp, tmp);
    Tau_cupti_set_offset(tmp);
    //Tau_cupti_set_offset((-1) * timestamp / 1e3);
    //cerr << "begining timestamp: " << TauTraceGetTimeStamp() - ((double)timestamp/1e3) << "ms.\n" << endl;
    //Tau_cupti_set_offset(0);

    Tau_gpu_init();
    Tau_cupti_setup_unified_memory();
}

void Tau_cupti_onunload() {
    if(TauEnv_get_cuda_track_unified_memory()) {
        CUPTI_CALL(cuptiActivityDisable(CUPTI_ACTIVITY_KIND_UNIFIED_MEMORY_COUNTER));
    }
}

extern "C" void Tau_metadata_task(char *name, const char* value, int tid);

/* TAU will create a unique virutal thread ID for each unique combination of:
 *  - Device
 *  - Context
 *  - Stream
 *  This way, asynchronous events will happen on the virtual thread in 
 *  monotonically increasing time order. */
int insert_context_into_map(uint32_t contextId, uint32_t deviceId, uint32_t streamId) {
    uint64_t key = (uint64_t)(contextId);
    key = (key << 32) + streamId;
    RtsLayer::LockDB();
    size_t c = newContextMap.count(key);
    RtsLayer::UnLockDB();
    int tid = 0;
    if (c == 0) {
        tau_cupti_context_t * tmp;
        tmp = new tau_cupti_context_t();
        tmp->contextId = contextId;
        tmp->deviceId = deviceId;
        tmp->streamId = streamId;
        tid = get_taskid_from_gpu_event(deviceId, streamId, contextId, false);
        tmp->v_threadId = tid;
        TAU_DEBUG_PRINT("key: %u, Device: %u = Context: %u = Stream: %u = Thread %u\n",
            key, deviceId, contextId, streamId, tid);
        RtsLayer::LockDB();
        newContextMap[key] = tmp;
        RtsLayer::UnLockDB();
    } else {
        RtsLayer::LockDB();
        tid = newContextMap[key]->v_threadId;
        RtsLayer::UnLockDB();
    }
    char tmpVal[32] = {0};
    sprintf(tmpVal, "%u", deviceId);
    Tau_metadata_task((char*)"CUPTI Device", tmpVal, tid);
    sprintf(tmpVal, "%u", contextId);
    Tau_metadata_task((char*)"CUPTI Context", tmpVal, tid);
    sprintf(tmpVal, "%u", streamId);
    Tau_metadata_task((char*)"CUPTI Stream", tmpVal, tid);
    return tid;
}

int get_vthread_for_cupti_context(const CUpti_ResourceData *handle, bool stream) {
    uint32_t contextId = 0;
    uint32_t deviceId = 0;
    uint32_t streamId = 0;
    int tid = 0;
    cuptiGetDeviceId(handle->context, &deviceId);
    cuptiGetContextId(handle->context, &contextId);
    if (stream) {
        uint8_t perThreadStream = 1;
        cuptiGetStreamIdEx(handle->context, handle->resourceHandle.stream, perThreadStream, &streamId);
    } else {
        while (contextId > context_null_streams.size()) {
            context_null_streams.push_back(0);
        }
        context_devices.push_back(deviceId);
    }
    return insert_context_into_map(contextId, deviceId, streamId);
}

/* Handler for Synchronous CUPTI_CB_DOMAIN_RESOURCE callbacks */

void Tau_handle_resource (void *ud, CUpti_CallbackDomain domain,
        CUpti_CallbackId id, const CUpti_ResourceData *handle) {
    TAU_DEBUG_PRINT("CUPTI_CB_DOMAIN_RESOURCE event\n");
    int tid = 0;
    switch (id) {
        case CUPTI_CBID_RESOURCE_INVALID: {
            TAU_DEBUG_PRINT("CUPTI_CBID_RESOURCE_INVALID\n");
            break;
            }
        case CUPTI_CBID_RESOURCE_CONTEXT_CREATED: {
            TAU_DEBUG_PRINT("CUPTI_CBID_RESOURCE_CONTEXT_CREATED\n");
            /*  I'd like to measure the lifetime of this context,
             *  but the mixing of synchronous and asynchronous events
             *  won't work with tracing.
            Tau_cupti_gpu_enter_event_from_cpu("CUDA Context", 
                get_vthread_for_cupti_context(handle, false));
                */
            deviceContextThreadVector.push_back(get_vthread_for_cupti_context(handle, false));
            break;
            }
        case CUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING: {
            TAU_DEBUG_PRINT("CUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING\n");
            /*  I'd like to measure the lifetime of this context,
             *  but the mixing of synchronous and asynchronous events
             *  won't work with tracing.
            Tau_cupti_gpu_exit_event_from_cpu("CUDA Context",
                get_vthread_for_cupti_context(handle, false));
                */
            get_vthread_for_cupti_context(handle, false);
            break;
            }
        case CUPTI_CBID_RESOURCE_STREAM_CREATED: {
            TAU_DEBUG_PRINT("CUPTI_CBID_RESOURCE_STREAM_CREATED\n");
            /*  I'd like to measure the lifetime of this stream,
             *  but the mixing of synchronous and asynchronous events
             *  won't work with tracing.
            Tau_cupti_gpu_enter_event_from_cpu("CUDA Stream",
                get_vthread_for_cupti_context(handle, true));
                */
            get_vthread_for_cupti_context(handle, true);
            break;
            }
        case CUPTI_CBID_RESOURCE_STREAM_DESTROY_STARTING: {
            TAU_DEBUG_PRINT("CUPTI_CBID_RESOURCE_STREAM_DESTROY_STARTING\n");
    uint32_t contextId = 0;
    uint32_t deviceId = 0;
    uint32_t streamId = 0;
    cuptiGetDeviceId(handle->context, &deviceId);
    cuptiGetContextId(handle->context, &contextId);
        uint8_t perThreadStream = 1;
        cuptiGetStreamIdEx(handle->context, handle->resourceHandle.stream, perThreadStream, &streamId);
            // printf("------------> Destroying Stream %d,%d,%d\n", deviceId, contextId, streamId);
            /*  I'd like to measure the lifetime of this stream,
             *  but the mixing of synchronous and asynchronous events
             *  won't work with tracing.
            Tau_cupti_gpu_exit_event_from_cpu("CUDA Stream",
                get_vthread_for_cupti_context(handle, true));
                */
            tid = get_vthread_for_cupti_context(handle, true);
            /* Very important!  We create a new virtual thread for each
             * stream.  When they go away, we need to reclaim the virtual
             * thread.  Ideally, in the async api we need to know when the
             * stream is created and destroyed, so we can put a timer around
             * it and the timeline reflects reality. Also, this recycling
             * should probably move to the async handler, so that we don't
             * give up the thread id until all the async events are in.
             */
            RtsLayer::recycleThread(tid);
            break;
            }
        case CUPTI_CBID_RESOURCE_CU_INIT_FINISHED: {
            TAU_DEBUG_PRINT("CUPTI_CBID_RESOURCE_CU_INIT_FINISHED\n");
            break;
            }
        case CUPTI_CBID_RESOURCE_MODULE_LOADED: {
            TAU_DEBUG_PRINT("CUPTI_CBID_RESOURCE_MODULE_LOADED\n");
            break;
            }
        case CUPTI_CBID_RESOURCE_MODULE_UNLOAD_STARTING: {
            TAU_DEBUG_PRINT("CUPTI_CBID_RESOURCE_MODULE_UNLOAD_STARTING\n");
            break;
            }
        case CUPTI_CBID_RESOURCE_MODULE_PROFILED: {
            TAU_DEBUG_PRINT("CUPTI_CBID_RESOURCE_MODULE_PROFILED\n");
            break;
            }
        case CUPTI_CBID_RESOURCE_SIZE:
        default:
        {
            TAU_DEBUG_PRINT("CUPTI_CBID_RESOURCE_SIZE\n");
            break;
        }
    }
    // if we want runtime cubin dump
    if(TauEnv_get_cuda_track_sass()) {
        if (map_disassem.empty()) {
            handleResource(id, handle);
        }
    }
}

 /* Handlers for synchronous Driver and Runtime callbacks */

void Tau_handle_driver_api_memcpy (void *ud, CUpti_CallbackDomain domain,
        CUpti_CallbackId id, const CUpti_CallbackData *cbInfo) {
    TAU_DEBUG_PRINT("TAU: CUPTI callback for memcpy\n");
    int kind;
    int count;
    get_values_from_memcpy(cbInfo, id, domain, kind, count);
    int taskId = get_taskid_from_context_id(cbInfo->contextUid, 0);
    if (cbInfo->callbackSite == CUPTI_API_ENTER) {
        CUptiResult cuptiErr;
        Tau_cupti_enter_memcpy_event( cbInfo->functionName, -1, 0,
            cbInfo->contextUid, cbInfo->correlationId, count,
            getMemcpyType(kind), taskId);
        Tau_cupti_register_host_calling_site(cbInfo->correlationId,
            cbInfo->functionName);
    } else { // memcpy exit
        TAU_DEBUG_PRINT("callback for %s, exit\n", cbInfo->functionName);
        CUptiResult cuptiErr;
        Tau_cupti_exit_memcpy_event( cbInfo->functionName, -1, 0,
            cbInfo->contextUid, cbInfo->correlationId, count,
            getMemcpyType(kind), taskId);
        if (function_is_sync(id)) { 
            TAU_DEBUG_PRINT("sync function name: %s\n", cbInfo->functionName);
            //Disable counter tracking during the sync.
            cudaDeviceSynchronize();
            record_gpu_counters_at_sync();

            // Why do we need to flush activity after every
            // synchronous memory transfer?
            // Tau_cupti_activity_flush_all();
        }
    }
}

void Tau_handle_driver_api_other (void *ud, CUpti_CallbackDomain domain,
        CUpti_CallbackId id, const CUpti_CallbackData *cbInfo) {
    if (cbInfo->callbackSite == CUPTI_API_ENTER)
    {
        if (function_is_exit(id))
        {
            //Do one last flush since this is our last opportunity.
#ifdef TAU_ASYNC_ACTIVITY_API
            cuptiActivityFlushAll(CUPTI_ACTIVITY_FLAG_NONE);
#endif
            //Stop collecting cupti counters.
#if !defined(PTHREADS) // SASS is not thread safe?
            if (TauEnv_get_cuda_track_sass()) {
                for (int i = 0; i < device_count_total; i++) {
                    for (std::map<uint32_t, CUpti_ActivityKernel4>::iterator it = kernelMap[i].begin(); 
                            it != kernelMap[i].end(); it++) {
                        uint32_t correlId = it->first;
                        CUpti_ActivityKernel4 *kernel = &it->second;
                        const char *kname = demangleName(kernel->name);
                        record_imix_counters(kname, i, kernel->streamId, kernel->contextId, kernel->correlationId, kernel->end);
                    }
                }
            }
#endif
            Tau_CuptiLayer_finalize();
        }
        if(strcmp(cbInfo->functionName, "cudaDeviceReset") == 0) {
            fprintf(stderr, "TAU: WARNING! cudaDeviceReset was called. CUPTI counters will not be measured from now on.\n");
        }
        Tau_gpu_enter_event(cbInfo->functionName);
        if (function_is_launch(id))
        { // ENTRY to a launch function
            Tau_CuptiLayer_init();

            TAU_DEBUG_PRINT("[at call (enter), %d] name: %s.\n", cbInfo->correlationId, cbInfo->functionName);
            record_gpu_launch(cbInfo->correlationId, cbInfo->functionName);
            CUdevice device;
            cuCtxGetDevice(&device);
            Tau_cuda_Event_Synchonize();
            int taskId = get_taskid_from_context_id(cbInfo->contextUid, 0);
            record_gpu_counters_at_launch(device, taskId);
        }
        TAU_DEBUG_PRINT("callback for %s, enter.\n", cbInfo->functionName);
    }
    else if (cbInfo->callbackSite == CUPTI_API_EXIT)
    {
        if (function_is_launch(id)) // EXIT FROM a launch function
        {
            record_gpu_launch(cbInfo->correlationId, cbInfo->functionName);
        }
#ifdef TAU_DEBUG_CUPTI_FORCE_SYNC
        //for testing only. 
        if (function_is_launch(id))
        {
            printf("synthetic sync point.\n");
            cuCtxSynchronize();
            FunctionInfo *p = TauInternal_CurrentProfiler(RtsLayer::myThread())->CallPathFunction;
        }
#endif

        TAU_DEBUG_PRINT("callback for %s, exit.\n", cbInfo->functionName);
        TAU_DEBUG_PRINT("[at call (exit), %d] name: %s.\n", cbInfo->correlationId, cbInfo->functionName);
        Tau_gpu_exit_event(cbInfo->functionName);
        if (function_is_sync(id))
        {
            TAU_DEBUG_PRINT("sync function name: %s\n", cbInfo->functionName);
            //Tau_CuptiLayer_disable();
            //cuCtxSynchronize();
            cudaDeviceSynchronize();
            //Tau_CuptiLayer_enable();
            record_gpu_counters_at_sync();

#ifdef TAU_ASYNC_ACTIVITY_API
            cuptiActivityFlushAll(CUPTI_ACTIVITY_FLAG_NONE);
#else
            Tau_cupti_register_sync_event(cbInfo->context, 0, NULL, 0, 0);
#endif
            //Tau_CuptiLayer_enable();
        }
    }
}

/* This callback handles synchronous things */

// Extra bool param that tells whether to run code
void Tau_cupti_callback_dispatch(void *ud, CUpti_CallbackDomain domain,
        CUpti_CallbackId id, const void *params) {
    // The get_device_count function causes a CUDA driver call.
    // If we're recieving callbacks for driver events, we have to not
    // process that call or we'll end up recursively callling get_device_count
    // forever (or until we run out of stack).
    if(disable_callbacks) {
        return;
    }
    if (!device_count_total) {
        RtsLayer::LockDB();
        disable_callbacks = 1;
        device_count_total = get_device_count();
        disable_callbacks = 0;
        RtsLayer::UnLockDB();
    }
    //Just in case we encounter a callback before TAU is intialized or finished.
    if (!Tau_init_check_initialized() || Tau_global_getLightsOut()) { 
        TAU_DEBUG_PRINT("TAU: [WARNING] Got CUPTI callback but TAU is either not yet initialized or has finished!\n");
        return;
    }
    if (domain == CUPTI_CB_DOMAIN_RESOURCE) {
        const CUpti_ResourceData *handle = (CUpti_ResourceData *) params;
        Tau_handle_resource (ud, domain, id, handle);
    }
    else if (domain == CUPTI_CB_DOMAIN_DRIVER_API || domain == CUPTI_CB_DOMAIN_RUNTIME_API) {
        const CUpti_CallbackData *cbInfo = (CUpti_CallbackData *) params;
        TAU_DEBUG_PRINT("in Tau_cupti_callback_dispatch: %s\n", cbInfo->functionName);
        if (cbInfo->context != NULL) {
            insert_correlation_id_map(cbInfo->correlationId, cbInfo->contextUid, 0);
        }
        if (function_is_memcpy(id, domain)) {
            // BEGIN handling memcpy
            Tau_handle_driver_api_memcpy (ud, domain, id, cbInfo);
        } else {
            // This is something other than memcpy
            Tau_handle_driver_api_other (ud, domain, id, cbInfo);
        }
    } else if (domain == CUPTI_CB_DOMAIN_INVALID) {
        TAU_DEBUG_PRINT("CUPTI_CB_DOMAIN_RESOURCE event\n");
    } else if (domain == CUPTI_CB_DOMAIN_SYNCHRONIZE) {
        TAU_DEBUG_PRINT("CUPTI_CB_DOMAIN_SYNCHRONIZE event\n");
        /* Todo: Count synchronization events? */
    } else if (domain == CUPTI_CB_DOMAIN_NVTX) {
        TAU_DEBUG_PRINT("CUPTI_CB_DOMAIN_NVTX event\n");
    } else if (domain == CUPTI_CB_DOMAIN_SIZE) {
        TAU_DEBUG_PRINT("CUPTI_CB_DOMAIN_SIZE event\n");
    } else {
        // do nothing
        return;
    }
}

    void CUPTIAPI Tau_cupti_activity_flush_all() {      
        if((Tau_CuptiLayer_get_num_events() > 0) || (buffers_queued++ > ACTIVITY_ENTRY_LIMIT)) {
            buffers_queued = 0;
            cuptiActivityFlushAll(CUPTI_ACTIVITY_FLAG_NONE);
        }
    }

    void CUPTIAPI Tau_cupti_register_sync_event(CUcontext context, uint32_t stream, uint8_t *activityBuffer, size_t size, size_t bufferSize)
    {
        static int counter = 0;
        int thisloop = counter++;
        // printf("------------> in %s, iteration %d\n", __func__, thisloop);
        //   if(TauEnv_get_cuda_track_sass()) {
        //     if(TauEnv_get_cuda_csv_output()) {
        // #ifdef TAU_DEBUG_CUPTI_SASS
        //       printf("[CuptiActivity]:  About to call createFilePointerSass, device_count: %i\n", device_count);
        // #endif
        //       createFilePointerSass(device_count);
        //     }
        //   }
        //Since we do not control the synchronization points this is only place where
        //we can record the gpu counters.
        int count_iter = TauEnv_get_cudaTotalThreads();
        record_gpu_counters_at_sync();

        //TAU_PROFILE("Tau_cupti_register_sync_event", "", TAU_DEFAULT);
        //printf("in sync: context=%p stream=%d.\n", context, stream);
        registered_sync = true;
        CUptiResult err, status;
        CUresult err2 = CUDA_SUCCESS;
        CUpti_Activity *record = NULL;
        //size_t bufferSize = 0;

        //start

#if defined(PTHREADS)
        if (count_iter > TAU_MAX_THREADS) {
            printf("TAU ERROR: Maximum number of threads (%d) exceeded. Please reconfigure TAU with -useropt=-DTAU_MAX_THREADS=3200 or some higher number\n", TAU_MAX_THREADS);
            exit(1);
        }
#else
        if (count_iter > TAU_MAX_GPU_DEVICES) {
            printf("TAU ERROR: Maximum number of devices (%d) exceeded. Please reconfigure TAU with -useropt=-DTAU_MAX_GPU_DEVICES=32 or some higher number\n", TAU_MAX_GPU_DEVICES);
            exit(1);
        }
#endif

        // for the ASYNC ACTIVITY API assume that the activityBuffer is vaild
        err = CUPTI_SUCCESS;
        //printf("err: %d.\n", err);

        uint64_t num_buffers = 0;
        if (err == CUPTI_SUCCESS)
        {
            //printf("succesfully dequeue'd buffer.\n");
            //TAU_START("next record loop");
            //TAU_PROFILE_TIMER(g, "getNextRecord", "", TAU_DEFAULT);
            //TAU_PROFILE_TIMER(r, "record_activity", "", TAU_DEFAULT);
            do {
                //TAU_PROFILE_START(g);
                status = cuptiActivityGetNextRecord(activityBuffer, bufferSize, &record);
                //TAU_PROFILE_STOP(g);
                if (status == CUPTI_SUCCESS) {
                    //TAU_PROFILE_START(r);
                    Tau_cupti_record_activity(record);
                    ++num_buffers;
                    //TAU_PROFILE_STOP(r);
                }
                else if (status == CUPTI_ERROR_MAX_LIMIT_REACHED) {
                    //const char *str;
                    //cuptiGetResultString(status, &str);
                    //printf("TAU ERROR: buffer limit encountered: %s.\n", str);
                    break;
                }
                else {
                    const char *str;
                    cuptiGetResultString(status, &str);
                    printf("TAU ERROR: cannot retrieve record from buffer: %s.\n", str);
                    break;
                }
            } while (status != CUPTI_ERROR_MAX_LIMIT_REACHED);
            //TAU_STOP("next record loop");

            size_t number_dropped;
            err = cuptiActivityGetNumDroppedRecords(NULL, 0, &number_dropped);

            if (number_dropped > 0)
                printf("TAU WARNING: %d CUDA records dropped, consider increasing the CUPTI_BUFFER size.", number_dropped);

            // With the ASYNC ACTIVITY API CUPTI will take care of calling
            // Tau_cupti_register_buffer_creation() when it needs a new activity buffer so
            // we are free to deallocate it here.
            free(activityBuffer);
            CUDA_CHECK_ERROR(err2, "Cannot requeue buffer.\n");


            for (int i=0; i < count_iter; i++) {
#ifdef TAU_DEBUG_CUPTI_COUNTERS
                printf("Kernels encountered/recorded: %d/%d.\n", kernels_encountered[i], kernels_recorded[i]);
#endif

                if (kernels_recorded[i] == kernels_encountered[i])
                {
                    clear_counters(i);
                    last_recorded_kernel_name = NULL;
                } else if (kernels_recorded[i] > kernels_encountered[i]) {
                    //printf("TAU: Recorded more kernels than were launched, exiting.\n");
                    //abort();
                    //exit(1);
                }
            }
            //     for (int i=0; i < device_count; i++) {
            // #ifdef TAU_DEBUG_CUPTI_COUNTERS
            //       printf("Kernels encountered/recorded: %d/%d.\n", kernels_encountered[i], kernels_recorded[i]);
            // #endif

            //       if (kernels_recorded[i] == kernels_encountered[i])
            //       {
            //         clear_counters(i);
            //         last_recorded_kernel_name = NULL;
            //       } else if (kernels_recorded[i] > kernels_encountered[i]) {
            //         //printf("TAU: Recorded more kernels than were launched, exiting.\n");
            //         //abort();
            //         //exit(1);
            //       }
            //     }
        } else if (err != CUPTI_ERROR_QUEUE_EMPTY) {
#ifdef TAU_DEBUG_CUPTI
            printf("TAU: CUPTI Activity queue is empty.\n");
            //CUDA_CHECK_ERROR(err2, "Cannot dequeue buffer.\n");
#endif
        } else if (err != CUPTI_ERROR_INVALID_PARAMETER) {
#ifdef TAU_DEBUG_CUPTI
            printf("TAU: CUPTI Invalid buffer");
            //CUDA_CHECK_ERROR(err2, "Cannot dequeue buffer, invalid buffer.\n");
#endif
        } else {
            printf("TAU: CUPTI Unknown error cannot read from buffer.\n");
        }
        // printf("------------> done in %s, iteration %d, %d buffers\n", __func__, thisloop, num_buffers);
    }

    void CUPTIAPI Tau_cupti_register_buffer_creation(uint8_t **activityBuffer, size_t *size, size_t *maxNumRecords)
    {
        uint8_t* bf = (uint8_t *)malloc(ACTIVITY_BUFFER_SIZE);
        if (bf == NULL) {
            printf("sufficient memory available to allocate activity buffer of size %d.", ACTIVITY_BUFFER_SIZE);
            exit(1);
        }
        *activityBuffer = bf;
        *size = ACTIVITY_BUFFER_SIZE;
        *maxNumRecords = 0;
    }

bool valid_sync_timestamp(uint64_t * start, uint64_t end, int taskId) {
    if (*start < previous_ts[taskId]) { 
        /* This is actually OK.  The synchronization could start before
         * the previous activity finished, that's the point.  We'll
         * move the start up to the previous end, and double check that
         * the synchronization end didn't exceed the previous activity.
         */
        if (end < previous_ts[taskId]) {
            /* OK, bad.  Dont' process this event. */
            sanity.sync_out_of_order++;
            TAU_DEBUG_PRINT("out of order: %f, previous %f\n", 
                (previous_ts[taskId] - *start)/1e3,
                previous_ts[taskId]/1e3); fflush(stdout);
            /* don't process this event? */
            return false;
        }
        *start = previous_ts[taskId];
    }
    return true;
}

    /* This callback handles asynchronous activity */

    void Tau_cupti_record_activity(CUpti_Activity *record)
    {
        CUptiResult err = CUPTI_SUCCESS;
        CUresult err2 = CUDA_SUCCESS;

        CUDA_CHECK_ERROR(err2, "Cannot get timestamp.\n");

        switch (record->kind) {
            case CUPTI_ACTIVITY_KIND_CONTEXT:
                {
                    CUpti_ActivityContext *context = (CUpti_ActivityContext *)record;
                    contextMap[context->deviceId] = *context;
                    currentContextId = context->contextId;
                    TAU_DEBUG_PRINT("----> Found null stream %d for context %d, device %d\n",
                        context->nullStreamId, context->contextId, context->deviceId);
                    context_null_streams[context->contextId] = context->nullStreamId;
                    context_devices[context->contextId] = context->deviceId;
                    break;
                }
                if (TauEnv_get_cuda_track_env()) {
#if CUDA_VERSION >= 5050
                    case CUPTI_ACTIVITY_KIND_ENVIRONMENT:
                        {
                            CUpti_ActivityEnvironment* env = (CUpti_ActivityEnvironment*)record;
#ifdef TAU_DEBUG_ENV
                            printf("ENVIRONMENT deviceId: %u, timestamp: %u\n", env->deviceId, env->timestamp);
#endif
                            double timestamp;
                            uint32_t deviceId;
                            uint16_t nullStreamId;
                            uint32_t contextId;
                            int context_idle = 5000; // reserved space for idle GPU monitoring

                            if (currentContextId == -1) {
                                contextId = context_idle + deviceId;
                            }
                            else {
                                contextId = currentContextId;
                            }
                            timestamp = env->timestamp/1e3;
                            deviceId = env->deviceId;

                            if (contextMap.empty()) {
                                nullStreamId = -1;
                                contextId = -1;
                            }
                            else {
                                nullStreamId = contextMap.find(deviceId)->second.nullStreamId;
                                contextId = contextMap.find(deviceId)->second.contextId;
                            }

                            if( environmentMap.find(contextId) == environmentMap.end() ) {
                                CudaEnvironment envt;
                                envt.timestamp.push_back(timestamp);
                                envt.deviceId = deviceId;
                                environmentMap[contextId] = envt;
                            }
                            else {
                                environmentMap.find(contextId)->second.timestamp.push_back(timestamp);
                            }

                            switch (env->environmentKind)
                            {
                                case CUPTI_ACTIVITY_ENVIRONMENT_SPEED:
                                    {
                                        uint32_t smClock = env->data.speed.smClock;
                                        uint32_t memoryClock = env->data.speed.memoryClock;
#ifdef TAU_DEBUG_ENV
                                        printf("SPEED\n");
                                        printf("\tsmClock = %d\n", smClock);
                                        printf("\tmemoryClock = %d\n", memoryClock);
#endif
                                        environmentMap.find(contextId)->second.smClock.push_back(smClock);
                                        environmentMap.find(contextId)->second.memoryClock.push_back(memoryClock);
                                        break;
                                    }
                                case CUPTI_ACTIVITY_ENVIRONMENT_TEMPERATURE:
                                    {
#ifdef TAU_DEBUG_ENV
                                        printf("TEMPERATURE = %d C\n", env->data.temperature.gpuTemperature);
#endif
                                        uint32_t gpuTemperature = env->data.temperature.gpuTemperature;
                                        environmentMap.find(contextId)->second.gpuTemperature.push_back(gpuTemperature);
                                        break;
                                    }
                                case CUPTI_ACTIVITY_ENVIRONMENT_POWER:
                                    {
#ifdef TAU_DEBUG_ENV
                                        printf("POWER\n");
                                        printf("\tpower: %u, power limit: %u\n", env->data.power.power, env->data.power.powerLimit);
#endif
                                        uint32_t power_t = env->data.power.power;
                                        uint32_t powerLimit = env->data.power.powerLimit;
                                        environmentMap.find(contextId)->second.power.push_back(power_t);
                                        environmentMap.find(contextId)->second.powerLimit = powerLimit; // cap shouldn't change
                                        double power_utilization = ((float)power_t/powerLimit) * 100.0;
                                        break;
                                    }
                                case CUPTI_ACTIVITY_ENVIRONMENT_COOLING:
                                    {
#ifdef TAU_DEBUG_ENV
                                        printf("COOLING\n");
                                        printf("\tfanspeed %u\n", env->data.cooling.fanSpeed);
#endif
                                        uint32_t fanSpeed = env->data.cooling.fanSpeed;
                                        environmentMap.find(contextId)->second.fanSpeed.push_back(fanSpeed);
                                        break;
                                    }
                                default:
#ifdef TAU_DEBUG_ENV
                                    printf("<unknown>\n");
#endif
                                    break;
                            }
                            break;
                        }
#endif
                }
            case CUPTI_ACTIVITY_KIND_MEMCPY:
#if CUDA_VERSION >= 5050
            case CUPTI_ACTIVITY_KIND_MEMCPY2:
#endif
                {
                    uint32_t deviceId;
                    uint32_t streamId;
                    uint32_t contextId;
                    uint64_t start;
                    uint64_t end;
                    uint64_t bytes;
                    uint8_t copyKind;
                    int id;
                    int direction = MESSAGE_UNKNOWN;
                    int taskId;

#if CUDA_VERSION >= 5050
                    if (record->kind == CUPTI_ACTIVITY_KIND_MEMCPY2) {
                        CUpti_ActivityMemcpy2 *memcpy = (CUpti_ActivityMemcpy2 *)record;
                        // are we getting events out of order? Ugh.
                        deviceId = memcpy->deviceId;
                        streamId = memcpy->streamId;
                        contextId = memcpy->contextId;
                        start = memcpy->start;
                        end = memcpy->end;
                        bytes = memcpy->bytes;
                        copyKind = memcpy->copyKind;
                        id = memcpy->correlationId;
#ifdef TAU_DEBUG_CUPTI
                        cerr << "recording memcpy (device, stream, context, correlation): " << memcpy->deviceId << ", " << memcpy->streamId << ", " << memcpy->contextId << ", " << memcpy->correlationId << ", " << memcpy->start << "-" << memcpy->end << "ns.\n" << endl;
                        cerr << "recording memcpy src: " << memcpy->srcDeviceId << "/" << memcpy->srcContextId << endl;
                        cerr << "recording memcpy dst: " << memcpy->dstDeviceId << "/" << memcpy->dstContextId << endl;
#endif
                        correlDeviceMap[id] = deviceId;
                        int taskId = get_taskid_from_context_id(contextId, streamId);
                        if (TauEnv_get_tracing() && start < previous_ts[taskId]) { 
                            sanity.memory_out_of_order++;
                        }
                        previous_ts[taskId] = end;
                        Tau_cupti_register_memcpy_event(
                                TAU_GPU_USE_DEFAULT_NAME,
                                memcpy->srcDeviceId,
                                streamId,
                                memcpy->srcContextId,
                                id,
                                start / 1e3,
                                end / 1e3,
                                bytes,
                                getMemcpyType(copyKind),
                                MESSAGE_RECIPROCAL_SEND, taskId
                                );
                        Tau_cupti_register_memcpy_event(
                                TAU_GPU_USE_DEFAULT_NAME,
                                memcpy->dstDeviceId,
                                streamId,
                                memcpy->dstContextId,
                                id,
                                start / 1e3,
                                end / 1e3,
                                bytes,
                                getMemcpyType(copyKind),
                                MESSAGE_RECIPROCAL_RECV, taskId
                                );
                    } else {
#endif
                        CUpti_ActivityMemcpy *memcpy = (CUpti_ActivityMemcpy *)record;
                        deviceId = memcpy->deviceId;
                        streamId = memcpy->streamId;
                        contextId = memcpy->contextId;
                        start = memcpy->start;
                        end = memcpy->end;
                        bytes = memcpy->bytes;
                        copyKind = memcpy->copyKind;
                        if (cupti_api_runtime())
                        {
                            id = memcpy->runtimeCorrelationId;
                        }
                        else
                        {
                            id = memcpy->correlationId;
                        }
                        if (getMemcpyType(copyKind) == MemcpyHtoD) {
                            direction = MESSAGE_RECV;
                        } else if (getMemcpyType(copyKind) == MemcpyDtoH) {
                            direction = MESSAGE_SEND;
                        }
                        correlDeviceMap[id] = deviceId;
                        int taskId = get_taskid_from_context_id(contextId, streamId);
                        if (TauEnv_get_tracing() && start < previous_ts[taskId]) { 
                            sanity.memory_out_of_order++;
                        }
                        previous_ts[taskId] = end;
#ifdef TAU_DEBUG_CUPTI
                        cerr << "recording memcpy: " << end - start << "ns.\n" << endl;
                        cerr << "recording memcpy on device: " << deviceId << endl;
                        cerr << "recording memcpy kind: " << getMemcpyType(copyKind) << endl;
#endif 
                        //We do not always know on the corresponding host event on
                        //the CPU what type of copy we have so we need to register 
                        //the bytes copied here. Be careful we only want to record 
                        //the bytes copied once.
                        Tau_cupti_register_memcpy_event(
                                TAU_GPU_USE_DEFAULT_NAME,
                                deviceId,
                                streamId,
                                contextId,
                                id,
                                start / 1e3,
                                end / 1e3,
                                bytes,
                                getMemcpyType(copyKind),
                                direction, taskId
                                );
                        /*
                           CuptiGpuEvent gId = CuptiGpuEvent(TAU_GPU_USE_DEFAULT_NAME, memcpy->streamId, memcpy->contextId, id, NULL, 0);
                        //cuptiGpuEvent cuRec = cuptiGpuEvent(TAU_GPU_USE_DEFAULT_NAME, &gId, NULL); 
                        Tau_gpu_register_memcpy_event(
                        &gId,
                        memcpy->start / 1e3, 
                        memcpy->end / 1e3, 
                        TAU_GPU_UNKNOW_TRANSFER_SIZE, 
                        getMemcpyType(memcpy->copyKind));
                        */	
#if CUDA_VERSION >= 5050
                    }
#endif

                    break;
                }

                if(TauEnv_get_cuda_track_unified_memory()) {
#if CUDA_VERSION >= 6000
                    case CUPTI_ACTIVITY_KIND_UNIFIED_MEMORY_COUNTER:
                        {	
                            CUpti_ActivityUnifiedMemoryCounterKind counterKind;
                            uint32_t deviceId;
                            uint32_t streamId;
                            uint32_t processId;
                            CUpti_ActivityUnifiedMemoryCounterScope scope;
                            uint64_t start;
                            uint64_t end;
                            uint64_t value;
                            int direction = MESSAGE_UNKNOWN;
                            CUpti_ActivityUnifiedMemoryCounter *umemcpy = (CUpti_ActivityUnifiedMemoryCounter *)record;

#ifdef TAU_DEBUG_CUPTI
#if CUDA_VERSION >= 7000
                            printf("UNIFIED_MEMORY_COUNTER [ %llu %llu ] kind=%s value=%llu src %u dst %u, streamId=%u\n",
                                    (unsigned long long)(umemcpy->start),
                                    (unsigned long long)(umemcpy->end),
                                    getUvmCounterKindString(umemcpy->counterKind),
                                    (unsigned long long)umemcpy->value,
                                    umemcpy->srcId,
                                    umemcpy->dstId,
                                    umemcpy->streamId);
#else
                            printf("UNIFIED_MEMORY_COUNTER [ %llu ], current stamp: %llu, scope=%d kind=%s value=%llu device %u\n",
                                    (unsigned long long)(umemcpy->timestamp), TauTraceGetTimeStamp(), 
                                    umemcpy->scope,
                                    getUvmCounterKindString(umemcpy->counterKind),
                                    (unsigned long long)umemcpy->value,
                                    umemcpy->deviceId);
#endif
#endif
                            counterKind = umemcpy->counterKind;
#if CUDA_VERSION < 7000
                            streamId = -1;
                            start = umemcpy->timestamp;
                            end = umemcpy->timestamp;
                            deviceId = umemcpy->deviceId;
#else
                            streamId = umemcpy->streamId;
                            start = umemcpy->start;
                            end=umemcpy->end;
                            deviceId = umemcpy->dstId;
#endif
                            processId = umemcpy->processId;
                            value = umemcpy->value;

                            if (getUnifmemType(counterKind) == BytesHtoD) {
                                direction = MESSAGE_RECV;
                            } else if (getUnifmemType(counterKind) == BytesDtoH) {
                                direction = MESSAGE_SEND;
                            }

                            //We do not always know on the corresponding host event on
                            //the CPU what type of copy we have so we need to register 
                            //the bytes copied here. Be careful we only want to record 
                            //the bytes copied once.
                            int taskId = deviceId; // need to get correlation id from CUpti_ActivityStream
                            Tau_cupti_register_unifmem_event(
                                    TAU_GPU_USE_DEFAULT_NAME,
                                    deviceId,
                                    streamId,
                                    processId,
                                    start,
                                    end,
                                    value,
                                    getUnifmemType(counterKind),
                                    direction,
                                    taskId
                                    );

                            break;
                        }
#endif
                }

            case CUPTI_ACTIVITY_KIND_KERNEL:
#if CUDA_VERSION >= 5000
            case CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL:
#endif
#if CUDA_VERSION >= 5050
            case CUPTI_ACTIVITY_KIND_CDP_KERNEL:
#endif
                {
                    const char *name;
                    uint32_t deviceId;
                    uint32_t streamId;
                    uint32_t contextId;
                    uint32_t correlationId;
#if CUDA_VERSION < 5050
                    uint32_t runtimeCorrelationId;
#endif
                    uint64_t start;
                    uint64_t end;
                    int64_t gridId;
                    int64_t parentGridId;
                    uint32_t blockX;
                    uint32_t blockY; 
                    uint32_t blockZ;
                    uint32_t dynamicSharedMemory;
                    uint32_t staticSharedMemory;
                    uint32_t localMemoryPerThread;
                    uint32_t registersPerThread;

#if CUDA_VERSION >= 5050
                    if (record->kind == CUPTI_ACTIVITY_KIND_CDP_KERNEL) {
                        printf(" inside cdp_kernel\n");
                        CUpti_ActivityCdpKernel *kernel = (CUpti_ActivityCdpKernel *)record;
                        name = kernel->name;
                        deviceId = kernel->deviceId;
                        streamId = kernel->streamId;
                        contextId = kernel->contextId;
                        correlationId = kernel->correlationId;
                        gridId = kernel->gridId;
                        parentGridId = kernel->parentGridId;
                        start = kernel->start;
                        end = kernel->end;
                        blockX = kernel->blockX;
                        blockY = kernel->blockY; 
                        blockZ = kernel->blockZ;
                        dynamicSharedMemory = kernel->dynamicSharedMemory;
                        staticSharedMemory = kernel->staticSharedMemory;
                        localMemoryPerThread = kernel->localMemoryPerThread;
                        registersPerThread = kernel->registersPerThread;
                    }
                    else {
#endif
                        CUpti_ActivityKernel4 *kernel = (CUpti_ActivityKernel4 *)record;
                        name = kernel->name;
                        deviceId = kernel->deviceId;
                        streamId = kernel->streamId;
                        contextId = kernel->contextId;
                        correlationId = kernel->correlationId;
#if CUDA_VERSION >= 7000
                        runtimeCorrelationId = correlationId;
                        gridId = kernel->gridId;
#elif CUDA_VERSION >= 5050 && CUDA_VERSION <= 6500
                        gridId = kernel->gridId;
                        runtimeCorrelationId = kernel->runtimeCorrelationId;
#else
                        gridId = 0;
                        runtimeCorrelationId = correlationId;
#endif
                        start = kernel->start;
                        end = kernel->end;
                        blockX = kernel->blockX;
                        blockY = kernel->blockY; 
                        blockZ = kernel->blockZ;
                        dynamicSharedMemory = kernel->dynamicSharedMemory;
                        staticSharedMemory = kernel->staticSharedMemory;
                        localMemoryPerThread = kernel->localMemoryPerThread;
                        registersPerThread = kernel->registersPerThread;
                        int taskId = get_taskid_from_context_id(kernel->contextId, streamId);
                        kernelMap[taskId][kernel->correlationId] = *kernel;
                        correlDeviceMap[kernel->correlationId] = kernel->deviceId;
#if CUDA_VERSION >= 5050
                    }
#endif
#ifdef TAU_DEBUG_CUPTI
                    cerr << "recording kernel (device, stream, context, correlation, grid, name): " << deviceId << ", " << streamId << ", " << contextId << ", " << correlationId << ", " << gridId << ", " << name << ", "<< start << "-" << end << "ns.\n" << endl;
#endif
                    uint32_t id;
                    if (cupti_api_runtime())
                    {
                        id = runtimeCorrelationId;
                    }
                    else
                    {
                        id = correlationId;
                    }
                    int taskId = get_taskid_from_context_id(contextId, streamId);
                        if (TauEnv_get_tracing() && start < previous_ts[taskId]) { 
                            sanity.kernel_out_of_order++;
                        }
                        previous_ts[taskId] = end;
                    // At this point store source locator and function maps accumulated, then clear maps
                    for (std::map<uint32_t, CUpti_ActivitySourceLocator>::iterator it = sourceLocatorMap.begin();
                            it != sourceLocatorMap.end();
                            it++) {
                        uint32_t srclocid = it->first;
                        CUpti_ActivitySourceLocator source = it->second;
                    }
                    eventMap[taskId].erase(eventMap[taskId].begin(), eventMap[taskId].end());
                    const char* name_og = name;
                    name = demangleName(name);
                    int number_of_metrics = Tau_CuptiLayer_get_num_events() + 1;
                    double metrics_start[number_of_metrics];
                    double metrics_end[number_of_metrics];
#if CUDA_VERSION >= 5050
                    if (record->kind != CUPTI_ACTIVITY_KIND_CDP_KERNEL) {
                        record_gpu_counters(taskId, name, id, &eventMap[taskId]);
                    }
#else
                    record_gpu_counters(taskId, name, id, &eventMap[taskId]);
#endif
                    if (TauEnv_get_cuda_track_env()) {
#if CUDA_VERSION >= 5050
                        record_environment_counters(name, taskId, deviceId, streamId, contextId, id, end);
#endif
                    }
                    if (gpu_occupancy_available(deviceId))
                    {
                        record_gpu_occupancy(blockX, 
                                blockY,
                                blockZ,
                                registersPerThread,
                                staticSharedMemory,
                                deviceId,
                                name, 
                                &eventMap[taskId]);
                        static TauContextUserEvent* bs;
                        static TauContextUserEvent* dm;
                        static TauContextUserEvent* sm;
                        static TauContextUserEvent* lm;
                        static TauContextUserEvent* lr;
                        Tau_get_context_userevent((void **) &bs, "Block Size");
                        Tau_get_context_userevent((void **) &dm, "Shared Dynamic Memory (bytes)");
                        Tau_get_context_userevent((void **) &sm, "Shared Static Memory (bytes)");
                        Tau_get_context_userevent((void **) &lm, "Local Memory (bytes per thread)");
                        Tau_get_context_userevent((void **) &lr, "Local Registers (per thread)");

                        eventMap[taskId][bs] = blockX * blockY * blockZ;
                        eventMap[taskId][dm] = dynamicSharedMemory;
                        eventMap[taskId][sm] = staticSharedMemory;
                        eventMap[taskId][lm] = localMemoryPerThread;
                        eventMap[taskId][lr] = registersPerThread;
                    }

                    GpuEventAttributes *map;
                    int map_size = eventMap[taskId].size();
                    map = (GpuEventAttributes *) malloc(sizeof(GpuEventAttributes) * map_size);
                    int i = 0;
                    for (eventMap_t::iterator it = eventMap[taskId].begin(); it != eventMap[taskId].end(); it++)
                    {
                        /*if(it->first == NULL) {
                          std::cerr << "Event was null!" << std::endl;
                          } else {
                          std::cerr << "Event was not null: " << it->first << std::endl;
                          }*/
                        //std::cerr << "event name: " << it->first->GetUserEventName() << std::endl;
                        map[i].userEvent = it->first;
                        map[i].data = it->second;
                        i++;
                    }

#if CUDA_VERSION >= 5050
                    if (record->kind == CUPTI_ACTIVITY_KIND_CDP_KERNEL) {
                        if (TauEnv_get_cuda_track_cdp()) {
                            Tau_cupti_register_gpu_event(name, deviceId,
                                    streamId, contextId, id, parentGridId, 
                                    true, map, map_size,
                                    start / 1e3, end / 1e3, taskId);
                        }
                    } else {
#endif
                        Tau_cupti_register_gpu_event(name, deviceId,
                                streamId, contextId, id, 0, false, map, map_size,
                                start / 1e3, end / 1e3, taskId);
#if CUDA_VERSION >= 5050
                    }
#endif
                    Tau_cupti_register_device_calling_site(gridId, name);
                    /*
                       CuptiGpuEvent gId = CuptiGpuEvent(name, kernel->streamId, kernel->contextId, id, map, map_size);
                    //cuptiGpuEvent cuRec = cuptiGpuEvent(name, &gId, &map);
                    Tau_gpu_register_gpu_event(
                    &gId, 
                    kernel->start / 1e3,
                    kernel->end / 1e3);
                    */	

                    break;
                }

            case CUPTI_ACTIVITY_KIND_DEVICE:
                {
                    CUpti_ActivityDevice *device = (CUpti_ActivityDevice *)record;

                    int nMeta = 17;

                    GpuMetadata *metadata = (GpuMetadata *) malloc(sizeof(GpuMetadata) * nMeta);
                    int id = 0;
                    //first the name.
                    metadata[id].name = (char*)("GPU Name");
                    metadata[id].value = device->name;
                    id++;

                    //the rest.
                    RECORD_DEVICE_METADATA(computeCapabilityMajor, device);
                    RECORD_DEVICE_METADATA(computeCapabilityMinor, device);
                    RECORD_DEVICE_METADATA(constantMemorySize, device);
                    RECORD_DEVICE_METADATA(coreClockRate, device);
                    RECORD_DEVICE_METADATA(globalMemoryBandwidth, device);
                    RECORD_DEVICE_METADATA(globalMemorySize, device);
                    RECORD_DEVICE_METADATA(l2CacheSize, device);
                    RECORD_DEVICE_METADATA(maxIPC, device);
                    RECORD_DEVICE_METADATA(maxRegistersPerBlock, device);
                    RECORD_DEVICE_METADATA(maxSharedMemoryPerBlock, device);
                    RECORD_DEVICE_METADATA(maxThreadsPerBlock, device);
                    RECORD_DEVICE_METADATA(maxWarpsPerMultiprocessor, device);
                    RECORD_DEVICE_METADATA(maxBlocksPerMultiprocessor, device);
                    RECORD_DEVICE_METADATA(numMemcpyEngines, device);
                    RECORD_DEVICE_METADATA(numMultiprocessors, device);
                    RECORD_DEVICE_METADATA(numThreadsPerWarp, device);

                    //cerr << "recording metadata (device): " << device->id << endl;
                    __deviceMap()[device->id] = *device;
#if CUDA_VERSION < 5000
                    if (__deviceMap().size() > 1 && Tau_CuptiLayer_get_num_events() > 0)
                    {
                        TAU_VERBOSE("TAU Warning: CUDA 5.0 or greater is needed to record counters on more that one GPU device at the same time.\n");
                    }
#endif
                    Tau_cupti_register_metadata(device->id, metadata, nMeta);
                    break;
                }
#if CUPTI_API_VERSION >= 3
            case CUPTI_ACTIVITY_KIND_SOURCE_LOCATOR:
                {
                    CUpti_ActivitySourceLocator *source = (CUpti_ActivitySourceLocator *)record;
                    sourceLocatorMap[source->id] = *source;
                    // uint32_t lineNumber;
#ifdef TAU_DEBUG_CUPTI
                    cerr << "source locator (id): " << source->id << ", " << source->fileName << ", " << source->lineNumber << ".\n" << endl;
#endif
                    srcLocMap[source->id] = *source;

#if CUDA_VERSION >= 5500
                    if(TauEnv_get_cuda_track_sass()) {

#ifdef TAU_DEBUG_CUPTI_SASS
                        printf("SOURCE_LOCATOR SrcLctrId: %d, File: %s, Line: %d, Kind: %u\n", 
                                source->id, source->fileName, source->lineNumber, source->kind);
#endif
                    }
#endif

                }

#if CUDA_VERSION >= 5500
            case CUPTI_ACTIVITY_KIND_INSTRUCTION_EXECUTION: {
                if(TauEnv_get_cuda_track_sass()) {
                    CUpti_ActivityInstructionExecution *instrRecord = (CUpti_ActivityInstructionExecution *)record;
                    int taskId = get_taskid_from_correlation_id(instrRecord->correlationId);
#ifdef TAU_DEBUG_CUPTI_SASS
                    printf("INSTRUCTION_EXECUTION corr: %u, executed: %u, flags: %u, functionId: %u, kind: %u, notPredOffThreadsExecuted: %u, pcOffset: %u, sourceLocatorId: %u, threadsExecuted: %u\n",
                    instrRecord->correlationId, instrRecord->executed, 
                    instrRecord->flags, instrRecord->functionId, 
                    instrRecord->kind, instrRecord->notPredOffThreadsExecuted,
                    instrRecord->pcOffset, instrRecord->sourceLocatorId, 
                    instrRecord->threadsExecuted);
#endif
                    instructionMap[taskId][instrRecord->correlationId].push_back(*instrRecord);
                }
                break;
            }
#endif
#if CUDA_VERSION >= 5500
            case CUPTI_ACTIVITY_KIND_FUNCTION: {
                if(TauEnv_get_cuda_track_sass()) {
                    CUpti_ActivityFunction *fResult = (CUpti_ActivityFunction *)record;
#ifdef TAU_DEBUG_CUPTI_SASS
                    printf("FUCTION contextId: %u, functionIndex: %u, id %u, kind: %u, moduleId %u, name %s, device: %i\n",
                        fResult->contextId, fResult->functionIndex, fResult->id,
                        fResult->kind, fResult->moduleId, fResult->name);
#endif
                    functionMap[fResult->id] = *fResult;
                }
                break;
            }
#endif
            case CUPTI_ACTIVITY_KIND_GLOBAL_ACCESS: {
                CUpti_ActivityGlobalAccess *global_access = (CUpti_ActivityGlobalAccess *)record;
#ifdef TAU_DEBUG_CUPTI
                cerr << "global access (cor. id) (source id): " << global_access->correlationId << ", " << global_access->sourceLocatorId << ", " << global_access->threadsExecuted << ".\n" << endl;
#endif
                int taskId = get_taskid_from_correlation_id(global_access->correlationId);
                CUpti_ActivityKernel4 *kernel = &kernelMap[taskId][global_access->correlationId];
                CUpti_ActivitySourceLocator *source = &sourceLocatorMap[global_access->sourceLocatorId];

                if (kernel->kind != CUPTI_ACTIVITY_KIND_INVALID) {
                    eventMap[taskId].erase(eventMap[taskId].begin(), eventMap[taskId].end());
                    std::string name;
                    form_context_event_name(kernel, source, "Accesses to Global Memory", &name);
                    TauContextUserEvent* ga;
                    Tau_cupti_find_context_event(&ga, name.c_str(), false);
                    eventMap[taskId][ga] = global_access->executed;
                    int map_size = eventMap[taskId].size();
                    GpuEventAttributes *map = (GpuEventAttributes *) malloc(sizeof(GpuEventAttributes) * map_size);
                    int i = 0;
                    for (eventMap_t::iterator it = eventMap[taskId].begin(); it != eventMap[taskId].end(); it++) {
                        map[i].userEvent = it->first;
                        map[i].data = it->second;
                        i++;
                    }
                    uint32_t id;
                    if (cupti_api_runtime()) {
#if CUDA_VERSION >= 6000 && CUDA_VERSION <= 6500
                        id = kernel->runtimeCorrelationId;
#else
                        id = kernel->correlationId;
#endif
                    } else {
                        id = kernel->correlationId;
                    }
                    Tau_cupti_register_gpu_atomic_event(demangleName(kernel->name), kernel->deviceId,
                    kernel->streamId, kernel->contextId, id, map, map_size, taskId);
                }
            }
            case CUPTI_ACTIVITY_KIND_BRANCH: {
                CUpti_ActivityBranch *branch = (CUpti_ActivityBranch *)record;
#ifdef TAU_DEBUG_CUPTI
                cerr << "branch (cor. id) (source id): " << branch->correlationId << ", " << branch->sourceLocatorId << ", " << branch->threadsExecuted << ".\n" << endl;
#endif
                int taskId = get_taskid_from_correlation_id(branch->correlationId);
                CUpti_ActivityKernel4 *kernel = &kernelMap[taskId][branch->correlationId];
                CUpti_ActivitySourceLocator *source = &sourceLocatorMap[branch->sourceLocatorId];
                if (kernel->kind != CUPTI_ACTIVITY_KIND_INVALID) {
                    eventMap[taskId].erase(eventMap[taskId].begin(), eventMap[taskId].end());
                    std::string name;
                    form_context_event_name(kernel, source, "Branches Executed", &name);
                    TauContextUserEvent* be;
                    Tau_cupti_find_context_event(&be, name.c_str(), false);
                    eventMap[taskId][be] = branch->executed;
                    form_context_event_name(kernel, source, "Branches Diverged", &name);
                    TauContextUserEvent* de;
                    Tau_cupti_find_context_event(&de, name.c_str(), false);
                    eventMap[taskId][de] = branch->diverged;
                    GpuEventAttributes *map;
                    int map_size = eventMap[taskId].size();
                    map = (GpuEventAttributes *) malloc(sizeof(GpuEventAttributes) * map_size);
                    int i = 0;
                    for (eventMap_t::iterator it = eventMap[taskId].begin(); it != eventMap[taskId].end(); it++) {
                        map[i].userEvent = it->first;
                        map[i].data = it->second;
                        i++;
                    }
                    uint32_t id;
                    if (cupti_api_runtime()) {
                        id = kernel->runtimeCorrelationId;
                    } else {
                        id = kernel->correlationId;
                    }
                    Tau_cupti_register_gpu_atomic_event(demangleName(kernel->name), kernel->deviceId,
                    kernel->streamId, kernel->contextId, id, map, map_size, taskId);
                }
            }
            case CUPTI_ACTIVITY_KIND_SYNCHRONIZATION: {
                CUpti_ActivitySynchronization *sync = (CUpti_ActivitySynchronization *)record;
                uint32_t streamId = 0;
                uint32_t deviceId = context_devices[sync->contextId];
                uint64_t start = sync->start;
                if (sync->streamId != CUPTI_SYNCHRONIZATION_INVALID_VALUE) {
                    streamId = sync->streamId;
                }
                int taskId = get_taskid_from_context_id(sync->contextId, streamId);
                switch (sync->type) {
                    case CUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_EVENT_SYNCHRONIZE: {
                        if ((!TauEnv_get_tracing() || streamId == 0) &&
                            (valid_sync_timestamp(&start, sync->end, taskId))) {
                            TAU_DEBUG_PRINT("Event Synchronize! c%u s%u o%u e%u t%u %f %f\n",
                                sync->contextId, streamId, sync->correlationId,
                                sync->cudaEventId, sync->type, start/1e3, sync->end/1e3);
                                fflush(stdout);
                            Tau_cupti_register_gpu_sync_event("Event Synchronize", deviceId,
                                streamId, sync->contextId, sync->correlationId,
                                start / 1e3, sync->end / 1e3, taskId);
                        }
                        break;
                    }
                    case CUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_STREAM_WAIT_EVENT: {
                        // Stream wait event API.
                        if ((!TauEnv_get_tracing()) &&
                            (valid_sync_timestamp(&start, sync->end, taskId))) {
                            TAU_DEBUG_PRINT("Stream Wait! c%u s%u o%u t%u %f %f\n",
                                sync->contextId, sync->streamId, sync->correlationId,
                                sync->type, start/1e3, sync->end/1e3);
                                fflush(stdout);
                            Tau_cupti_register_gpu_sync_event("Stream Wait", deviceId,
                                streamId, sync->contextId, sync->correlationId,
                                start / 1e3, sync->end / 1e3, taskId);
                        }
                        break;
                    }
                    case CUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_STREAM_SYNCHRONIZE: {
                        // Stream synchronize API.
                        if ((!TauEnv_get_tracing()) &&
                            (valid_sync_timestamp(&start, sync->end, taskId))) {
                            TAU_DEBUG_PRINT("Stream Synchronize! c%u s%u o%u t%u %f %f\n",
                                sync->contextId, sync->streamId, sync->correlationId,
                                sync->type, start/1e3, sync->end/1e3);
                                fflush(stdout);
                            Tau_cupti_register_gpu_sync_event("Stream Synchronize", deviceId,
                                streamId, sync->contextId, sync->correlationId,
                                start / 1e3, sync->end / 1e3, taskId);
                        }
                        break;
                    }
                    case CUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_CONTEXT_SYNCHRONIZE: {
                        /* Context/Device synchronize API. */
                        if (valid_sync_timestamp(&start, sync->end, taskId)) {
                            TAU_DEBUG_PRINT("Context Synchronize! c%u o%u t%u %f %f\n",
                                sync->contextId, sync->correlationId,
                                sync->type, start/1e3, sync->end/1e3);
                            Tau_cupti_register_gpu_sync_event("Context Synchronize", deviceId,
                                streamId, sync->contextId, sync->correlationId,
                                start / 1e3, sync->end / 1e3, taskId);
                        }
                        break;
                    }
                    default:
                        break;
                }
                previous_ts[taskId] = sync->end;
            }
#endif //CUPTI_API_VERSION >= 3
        }
    }

    //Helper function givens ceiling with given significance.
    int ceil(float value, int significance)
    {
        return ceil(value/significance)*significance;
    }

    int gpu_occupancy_available(int deviceId)
    { 
        //device callback not called.
        if (__deviceMap().empty())
        {
            return 0;
        }

        CUpti_ActivityDevice device = __deviceMap()[deviceId];

        if ((device.computeCapabilityMajor > 7) ||
                device.computeCapabilityMajor == 7 &&
                device.computeCapabilityMinor > 1)
        {
            TAU_VERBOSE("TAU Warning: GPU occupancy calculator is not implemented for devices of compute capability > 7.1.");
            return 0;
        }
        //gpu occupancy available.
        return 1;	
    }
    int gpu_source_locations_available()
    {
        //always available. 
        return 1;
    }

    void transport_imix_counters(uint32_t vec, Instrmix imixT, const char* name, uint32_t deviceId, uint32_t streamId, uint32_t contextId, uint32_t id, uint64_t end, TauContextUserEvent * tc)
    {
        int taskId = get_taskid_from_context_id(contextId, streamId);
        eventMap[taskId][tc] = vec;

        GpuEventAttributes *map;
        int map_size = eventMap[taskId].size();
        map = (GpuEventAttributes *) malloc(sizeof(GpuEventAttributes) * map_size);
        int i = 0;

        for (eventMap_t::iterator it = eventMap[taskId].begin(); it != eventMap[taskId].end(); it++) {
            map[i].userEvent = it->first;
            map[i].data = it->second;
            i++;
        }
        // transport
        Tau_cupti_register_gpu_event(name, deviceId,
                streamId, contextId, id, 0, false, map, map_size,
                end / 1e3, end / 1e3, taskId);
    }

    void record_imix_counters(const char* name, uint32_t deviceId, uint32_t streamId, uint32_t contextId, uint32_t id, uint64_t end) {
        // check if data available
        bool update = false;
        int taskId = get_taskid_from_context_id(contextId, streamId);
        if (instructionMap[taskId].find(id) == instructionMap[taskId].end()) {
            TAU_VERBOSE("[CuptiActivity] warning:  Instruction mix counters not recorded.\n");
        }
        else if (map_disassem.empty()) {
            TAU_VERBOSE("[CuptiActivity] warning:  No disassembly found, SASS counters not recorded.\n");
        }
        else {
            ImixStats is_runtime = write_runtime_imix(id, taskId, map_disassem, name);
#ifdef TAU_DEBUG_CUPTI
            cout << "[CuptiActivity]:  Name: " << name << 
                ", FLOPS_raw: " << is_runtime.flops_raw << ", MEMOPS_raw: " << is_runtime.memops_raw <<
                ", CTRLOPS_raw: " << is_runtime.ctrlops_raw << ", TOTOPS_raw: " << is_runtime.totops_raw << ".\n";
#endif
            update = true;
            static TauContextUserEvent* fp_ops;
            static TauContextUserEvent* mem_ops;
            static TauContextUserEvent* ctrl_ops;

            Tau_get_context_userevent((void **) &fp_ops, "Floating Point Operations");
            Tau_get_context_userevent((void **) &mem_ops, "Memory Operations");
            Tau_get_context_userevent((void **) &ctrl_ops, "Control Operations");

            uint32_t  v_flops = is_runtime.flops_raw;
            uint32_t v_memops = is_runtime.memops_raw;
            uint32_t v_ctrlops = is_runtime.totops_raw;


            transport_imix_counters(v_flops, FlPtOps, name, deviceId, streamId, contextId, id, end, fp_ops);
            transport_imix_counters(v_memops, MemOps, name, deviceId, streamId, contextId, id, end, mem_ops);
            transport_imix_counters(v_ctrlops, CtrlOps, name, deviceId, streamId, contextId, id, end, ctrl_ops);

            // Each time imix counters recorded, erase instructionMap.
            std::map<uint32_t, std::list<CUpti_ActivityInstructionExecution> >::iterator it_temp = instructionMap[taskId].find(id);
            instructionMap[taskId].erase(it_temp);
            eventMap[taskId].erase(eventMap[taskId].begin(), eventMap[taskId].end());
        }
        if(!update) {
            TAU_VERBOSE("TAU Warning:  Did not record instruction operations.\n");
        }

    }

    ImixStats write_runtime_imix(uint32_t corrId, uint32_t taskId, std::map<std::pair<int, int>, CudaOps> map_disassem, std::string kernel)
    {

#ifdef TAU_DEBUG_SASS
        cout << "[CuptiActivity]: write_runtime_imix begin\n";
#endif

        // look up from map_imix_static
        ImixStats imix_stats;
        string current_kernel = "";
        int flops_raw = 0;
        int ctrlops_raw = 0;
        int memops_raw = 0;
        int totops_raw = 0;
        double flops_pct = 0;
        double ctrlops_pct = 0;
        double memops_pct = 0;
        std::list<CUpti_ActivityInstructionExecution> instrSampList = instructionMap[taskId].find(corrId)->second;

        if (!instrSampList.empty()) {
            for (std::list<CUpti_ActivityInstructionExecution>::iterator iter=instrSampList.begin();
                    iter != instrSampList.end(); 
                    iter++) {
                CUpti_ActivityInstructionExecution is = *iter;
                int sid = is.sourceLocatorId;
                int lineno = -1;
                if ( srcLocMap.find(sid) != srcLocMap.end() ) {
                    lineno = srcLocMap.find(sid)->second.lineNumber;
                    std::pair<int, int> p1 = std::make_pair(lineno, (unsigned int) is.pcOffset);

                    for (std::map<std::pair<int, int>,CudaOps>::iterator iter= map_disassem.begin();
                            iter != map_disassem.end(); iter++) { 
                        CudaOps cuops = iter->second;
                        if (map_disassem.find(p1) != map_disassem.end()) {
                            CudaOps cuops = map_disassem.find(p1)->second;
                            int instr_type = get_instruction_mix_category(cuops.instruction);
                            switch(instr_type) {
                                // Might be non-existing ops, don't count those!
                                case FloatingPoint: case Integer:
                                case SIMD: case Conversion: {
                                                                flops_raw++;
                                                                totops_raw++;
                                                                break;
                                                            }
                                case LoadStore: case Texture:
                                case Surface: {
                                                  memops_raw++;
                                                  totops_raw++;
                                                  break;
                                              }
                                case Control: case Move:
                                case Predicate: {
                                                    ctrlops_raw++;
                                                    totops_raw++;
                                                    break;
                                                }
                                case Misc: {
                                               totops_raw++;
                                               break;
                                           }
                            }
                        }
                        else {
#if TAU_DEBUG_DISASM
                            cout << "[CuptiActivity]:  map_disassem does not exist for pair(" 
                                << lineno << "," << is->pcOffset << ")\n";
#endif
                        }
                    }
                }
                else {
#if TAU_DEBUG_DISASM
                    cout << "[CuptiActivity]:  srcLocMap does not exist for sid: " << sid << endl;
#endif
                }
            }
        }
        else {
            cout << "[CuptiActivity]: instrSamp_list empty!\n";
        }

        string kernel_iter = kernel;

        flops_pct = ((float)flops_raw/totops_raw) * 100;
        memops_pct = ((float)memops_raw/totops_raw) * 100;
        ctrlops_pct = ((float)ctrlops_raw/totops_raw) * 100;
        // push onto map
        imix_stats.flops_raw = flops_raw;
        imix_stats.ctrlops_raw = ctrlops_raw;
        imix_stats.memops_raw = memops_raw;
        imix_stats.totops_raw = totops_raw;
        imix_stats.flops_pct = flops_pct;
        imix_stats.ctrlops_pct = ctrlops_pct;
        imix_stats.memops_pct = memops_pct;
        imix_stats.kernel = kernel_iter;

#ifdef TAU_DEBUG_DISASM
        cout << "[CudaDisassembly]:  current_kernel: " << kernel_iter << endl;
        cout << "  FLOPS: " << flops_raw << ", MEMOPS: " << memops_raw 
            << ", CTRLOPS: " << ctrlops_raw << ", TOTOPS: " << totops_raw << "\n";
        cout << setprecision(2) << "  FLOPS_pct: " << flops_pct << "%, MEMOPS_pct: " 
            << memops_pct << "%, CTRLOPS_pct: " << ctrlops_pct << "%\n";
#endif

        return imix_stats;
    }

    void transport_environment_counters(std::vector<uint32_t> vec, EnvType envT, const char* name, uint32_t taskId, uint32_t deviceId, uint32_t streamId, uint32_t contextId, uint32_t id, uint64_t end, TauContextUserEvent* tc)
    {
        if (vec.size()==0) {
            eventMap[taskId][tc] = 0;
        }
        else {
            for(std::vector<uint32_t>::iterator iter=vec.begin(); iter != vec.end(); iter++) {
                if (envT == PowerUtilization) {
                    uint32_t power_lim = environmentMap.find(contextId)->second.powerLimit;
                    eventMap[taskId][tc] = ((float)*iter/power_lim)*100.0;
                }
                else {
                    eventMap[taskId][tc] = *iter;
                }

                GpuEventAttributes *map;
                int map_size = eventMap[taskId].size();
                map = (GpuEventAttributes *) malloc(sizeof(GpuEventAttributes) * map_size);
                int i = 0;

                for (eventMap_t::iterator it = eventMap[taskId].begin(); it != eventMap[taskId].end(); it++) {	
                    map[i].userEvent = it->first;
                    map[i].data = it->second;
                    i++;
                }
                Tau_cupti_register_gpu_event(name, deviceId,
                        streamId, contextId, id, 0, false, map, map_size,
                        end / 1e3, end / 1e3, taskId);
            }
        }

    }

    void record_environment_counters(const char* name, uint32_t taskId, uint32_t deviceId, uint32_t streamId, uint32_t contextId, uint32_t id, uint32_t end) {
        if (environmentMap.find(contextId) == environmentMap.end()) {
            TAU_VERBOSE("[CuptiActivity] warning:  GPU environment counters not recorded.\n");
        }
        else {
            static TauContextUserEvent* sm_clock;
            static TauContextUserEvent* memory_clock;
            static TauContextUserEvent* gpu_temperature;
            static TauContextUserEvent* power_t;
            static TauContextUserEvent* fan_speed;

            Tau_get_context_userevent((void **) &sm_clock, "SM Frequency (MHz)");
            Tau_get_context_userevent((void **) &memory_clock, "Memory Frequency (MHz)");
            Tau_get_context_userevent((void **) &gpu_temperature, "GPU Temperature (C)");
            Tau_get_context_userevent((void **) &power_t, "Power Utilization (% mW)");
            Tau_get_context_userevent((void **) &fan_speed, "Fan Speed (% max)");

            CudaEnvironment ce = environmentMap.find(contextId)->second;
            std::vector<uint32_t> v_power = ce.power;
            std::vector<uint32_t> v_smClock = ce.smClock;
            std::vector<uint32_t> v_memoryClock = ce.memoryClock;
            std::vector<uint32_t> v_gpuTemperature = ce.gpuTemperature;
            std::vector<uint32_t> v_fanSpeed = ce.fanSpeed;

            transport_environment_counters(v_power, PowerUtilization, name, deviceId, taskId, streamId, contextId, id, end, power_t);
            transport_environment_counters(v_smClock, SMClock, name, deviceId, taskId, streamId, contextId, id, end, sm_clock);
            transport_environment_counters(v_memoryClock, MemoryClock, name, taskId, deviceId, streamId, contextId, id, end, memory_clock);
            transport_environment_counters(v_gpuTemperature, GPUTemperature, name, taskId, deviceId, streamId, contextId, id, end, gpu_temperature);
            transport_environment_counters(v_fanSpeed, FanSpeed, name, taskId, deviceId, streamId, contextId, id, end, fan_speed);
        }
    }

    void record_gpu_launch(int correlationId, const char *name)
    {
#ifdef TAU_DEBUG_CUPTI
        printf("TAU: CUPTI recording GPU launch: %s\n", name);
#endif
        Tau_cupti_register_host_calling_site(correlationId, name);	
    }

    void record_gpu_counters(int device_id, const char *name, uint32_t correlationId, eventMap_t *m)
    {
        int taskId = get_taskid_from_correlation_id(correlationId);
        if (Tau_CuptiLayer_get_num_events() > 0 &&
                !counters_bounded_warning_issued[taskId] && 
                last_recorded_kernel_name != NULL && 
                strcmp(last_recorded_kernel_name, name) != 0) 
        {
            TAU_VERBOSE("TAU Warning: CUPTI events will be bounded, multiple different kernel deteched between synchronization points.\n");
            counters_bounded_warning_issued[taskId] = true;
            for (int n = 0; n < Tau_CuptiLayer_get_num_events(); n++) {
                Tau_CuptiLayer_set_event_name(n, TAU_CUPTI_COUNTER_BOUNDED); 
            }
        }
        last_recorded_kernel_name = name;
        {
            //increment kernel count.

            for (int n = 0; n < Tau_CuptiLayer_get_num_events(); n++) {
#ifdef TAU_DEBUG_CUPTI_COUNTERS
                std::cout << "at record: "<< name << " ====> " << std::endl;
                std::cout << "\tstart: " << counters_at_last_launch[taskId][n] << std::endl;
                std::cout << "\t stop: " << current_counters[taskId][n] << std::endl;
#endif
                TauContextUserEvent* c;
                const char *name = Tau_CuptiLayer_get_event_name(n);
                if (n >= counterEvents[taskId].size()) {
                    c = (TauContextUserEvent *) Tau_return_context_userevent(name);
                    counterEvents[taskId].push_back(c);
                } else {
                    c = counterEvents[taskId][n];
                }
                Tau_set_context_event_name(c, name);
                if (counters_averaged_warning_issued[taskId] == true)
                {
                    eventMap[taskId][c] = (current_counters[taskId][n] - counters_at_last_launch[taskId][n]);
                }
                else {
                    eventMap[taskId][c] = (current_counters[taskId][n] - counters_at_last_launch[taskId][n]) * kernels_encountered[taskId];
                }

            }
            kernels_recorded[taskId]++;
        }
    }
    void record_gpu_occupancy(int32_t blockX, 
            int32_t blockY,
            int32_t blockZ,
            uint16_t registersPerThread,
            int32_t staticSharedMemory,
            uint32_t deviceId,
            const char *name, 
            eventMap_t *eventMap)
    {
        CUpti_ActivityDevice device = __deviceMap()[deviceId];


        int myWarpsPerBlock = (int)ceil(
                (double)(blockX * blockY * blockZ)/
                (double)(device.numThreadsPerWarp)
                ); 

        int allocatable_warps = min(
                (int)device.maxBlocksPerMultiprocessor, 
                (int)floor(
                    (float) device.maxWarpsPerMultiprocessor/
                    myWarpsPerBlock	
                    )
                );

        static TauContextUserEvent* alW;
        Tau_get_context_userevent((void **) &alW, "Allocatable Blocks per SM given Thread count (Blocks)");
        (*eventMap)[alW] = allocatable_warps;
        //eventMap[5].userEvent = alW;
        //eventMap[5].data = allocatable_warps;

        int myRegistersPerBlock = device.computeCapabilityMajor < 2 ?
            ceil(
                    ceil(
                        (float)myWarpsPerBlock, 2	
                        )*
                    registersPerThread*
                    device.numThreadsPerWarp,
                    device.computeCapabilityMinor < 2 ? 256 : 512
                ) :
            ceil(
                    registersPerThread*
                    device.numThreadsPerWarp,
                    device.computeCapabilityMajor < 3 ? 128 : 256
                )*
            ceil(
                    myWarpsPerBlock, device.computeCapabilityMajor < 3 ? 2 : 4
                );

        int allocatable_registers = (int)floor(
                device.maxRegistersPerBlock/
                max(
                    myRegistersPerBlock, 1
                   )
                );

        if (allocatable_registers == 0)
            allocatable_registers = device.maxBlocksPerMultiprocessor;


        static TauContextUserEvent* alR;
        Tau_get_context_userevent((void **) &alR, "Allocatable Blocks Per SM given Registers used (Blocks)");
        (*eventMap)[alR] = allocatable_registers;

        int sharedMemoryUnit;
        switch(device.computeCapabilityMajor)
        {
            case 1: sharedMemoryUnit = 512; break;
            case 2: sharedMemoryUnit = 128; break;
            case 3: case 5: case 6: case 7: sharedMemoryUnit = 256; break;
        }
        int mySharedMemoryPerBlock = ceil(
                staticSharedMemory,
                sharedMemoryUnit
                );

        int allocatable_shared_memory = mySharedMemoryPerBlock > 0 ?
            floor(
                    device.maxSharedMemoryPerBlock/
                    mySharedMemoryPerBlock
                 ) :
            device.maxThreadsPerBlock
            ;

        static TauContextUserEvent* alS;
        Tau_get_context_userevent((void **) &alS, "Allocatable Blocks Per SM given Shared Memory usage (Blocks)");
        (*eventMap)[alS] = allocatable_shared_memory;

        int allocatable_blocks = min(allocatable_warps, min(allocatable_registers, allocatable_shared_memory));

        int occupancy = myWarpsPerBlock * allocatable_blocks;

        // #define RESULTS_TO_STDOUT 1
#ifdef RESULTS_TO_STDOUT
        printf("[%s] occupancy calculator:\n", name);

        printf("myWarpsPerBlock            = %d.\n", myWarpsPerBlock);
        printf("allocatable warps          = %d.\n", allocatable_warps);
        printf("myRegistersPerBlock        = %d.\n", myRegistersPerBlock);
        printf("allocatable registers      = %d.\n", allocatable_registers);
        printf("mySharedMemoryPerBlock     = %d.\n", mySharedMemoryPerBlock);
        printf("allocatable shared memory  = %d.\n", allocatable_shared_memory);

        printf("              >> occupancy = %d (%2.0f%% of %d).\n", 
                occupancy, ((float)occupancy/device.maxWarpsPerMultiprocessor)*100, device.maxWarpsPerMultiprocessor);
#endif

        static TauContextUserEvent* occ;
        Tau_get_context_userevent((void **) &occ, "GPU Occupancy (Warps)");
        (*eventMap)[occ] = occupancy;

    }

#if CUPTI_API_VERSION >= 3
    void form_context_event_name(CUpti_ActivityKernel4 *kernel, CUpti_ActivitySourceLocator *source, const char *event_name, std::string *name)
    {         

        stringstream file_and_line("");
        file_and_line << event_name << " : ";
        file_and_line << demangleName(kernel->name);
        if (source->kind != CUPTI_ACTIVITY_KIND_INVALID)
        {
            file_and_line << " => [{" << source->fileName   << "}";
            file_and_line <<  " {" << source->lineNumber << "}]";
        }

        *name = file_and_line.str();

        //cout << "file and line: " << file_and_line.str() << endl;

    }
#endif // CUPTI_API_VERSION >= 3


    bool function_is_sync(CUpti_CallbackId id)
    {
        return (	
                //unstable results otherwise(
                //runtimeAPI
                //id == CUPTI_RUNTIME_TRACE_CBID_cudaFree_v3021 ||
                //id == CUPTI_RUNTIME_TRACE_CBID_cudaFreeArray_v3020 ||
                //id == CUPTI_RUNTIME_TRACE_CBID_cudaFreeHost_v3020
                //id == CUPTI_RUNTIME_TRACE_CBID_cudaEventRecord_v3020
                //id == CUPTI_RUNTIME_TRACE_CBID_cudaThreadExit_v3020 || 
                //id == CUPTI_RUNTIME_TRACE_CBID_cudaDeviceReset_v3020 ||
            id == CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy_v3020 ||
            id == CUPTI_RUNTIME_TRACE_CBID_cudaEventSynchronize_v3020 ||
            //id == CUPTI_RUNTIME_TRACE_CBID_cudaEventQuery_v3020 ||
            //driverAPI
            id == CUPTI_DRIVER_TRACE_CBID_cuMemcpy_v2 ||
            id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD_v2 ||
            id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH_v2 ||
            id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoD_v2 ||
            id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoH_v2 ||
            id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoD_v2 ||
            id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoA_v2 ||
            id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoA_v2 ||
            id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoA_v2 ||
            id == CUPTI_DRIVER_TRACE_CBID_cuEventSynchronize //||
            //id == CUPTI_DRIVER_TRACE_CBID_cuEventQuery

            );
    }
    bool function_is_exit(CUpti_CallbackId id)
    {

        return (
                id == CUPTI_RUNTIME_TRACE_CBID_cudaThreadExit_v3020 || 
                id == CUPTI_RUNTIME_TRACE_CBID_cudaDeviceReset_v3020
                //driverAPI
               );

    }
    bool function_is_launch(CUpti_CallbackId id) { 
        return id == CUPTI_RUNTIME_TRACE_CBID_cudaLaunch_v3020
            || id == CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel
#if CUDA_VERSION >= 7000
            || id == CUPTI_RUNTIME_TRACE_CBID_cudaLaunchKernel_v7000
            || id == CUPTI_RUNTIME_TRACE_CBID_cudaLaunch_ptsz_v7000 
            || id == CUPTI_RUNTIME_TRACE_CBID_cudaLaunchKernel_ptsz_v7000
#endif
            ;

    }

    bool function_is_memcpy(CUpti_CallbackId id, CUpti_CallbackDomain domain) {
        if (domain == CUPTI_CB_DOMAIN_RUNTIME_API)
        {
            return (
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy_v3020 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToArray_v3020 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromArray_v3020 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyArrayToArray_v3020 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToSymbol_v3020 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromSymbol_v3020 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyAsync_v3020 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToArrayAsync_v3020 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromArrayAsync_v3020 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToSymbolAsync_v3020 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromSymbolAsync_v3020 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyPeer_v4000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyPeerAsync_v4000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3DPeer_v4000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3DPeerAsync_v4000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2D_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToArray_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DToArray_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromArray_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DFromArray_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyArrayToArray_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DArrayToArray_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToSymbol_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromSymbol_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyAsync_ptsz_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToArrayAsync_ptsz_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromArrayAsync_ptsz_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DAsync_ptsz_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DToArrayAsync_ptsz_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DFromArrayAsync_ptsz_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToSymbolAsync_ptsz_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromSymbolAsync_ptsz_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3D_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3DAsync_ptsz_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3DPeer_ptds_v7000 ||
                    id ==     CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3DPeerAsync_ptsz_v7000
                   );
        }
        else if (domain == CUPTI_CB_DOMAIN_DRIVER_API)
        {
            return (
                    id ==     CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD_v2 ||
                    id ==     CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH_v2 ||
                    id ==     CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoDAsync_v2 ||
                    id ==     CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoHAsync_v2
                   );
        }
        else
        {
            return false;
        }
    }

    void get_values_from_memcpy(const CUpti_CallbackData *info, CUpti_CallbackId id, CUpti_CallbackDomain domain, int &kind, int &count)
    {
        if (domain == CUPTI_CB_DOMAIN_RUNTIME_API)
        {
            CAST_TO_RUNTIME_MEMCPY_TYPE_AND_CALL(cudaMemcpy, id, info, kind, count)
                CAST_TO_RUNTIME_MEMCPY_TYPE_AND_CALL(cudaMemcpyToArray, id, info, kind, count)
                CAST_TO_RUNTIME_MEMCPY_TYPE_AND_CALL(cudaMemcpyFromArray, id, info, kind, count)
                CAST_TO_RUNTIME_MEMCPY_TYPE_AND_CALL(cudaMemcpyArrayToArray, id, info, kind, count)
                CAST_TO_RUNTIME_MEMCPY_TYPE_AND_CALL(cudaMemcpyToSymbol, id, info, kind, count)
                CAST_TO_RUNTIME_MEMCPY_TYPE_AND_CALL(cudaMemcpyFromSymbol, id, info, kind, count)
                CAST_TO_RUNTIME_MEMCPY_TYPE_AND_CALL(cudaMemcpyAsync, id, info, kind, count)
                CAST_TO_RUNTIME_MEMCPY_TYPE_AND_CALL(cudaMemcpyToArrayAsync, id, info, kind, count)
                CAST_TO_RUNTIME_MEMCPY_TYPE_AND_CALL(cudaMemcpyFromArrayAsync, id, info, kind, count)
                CAST_TO_RUNTIME_MEMCPY_TYPE_AND_CALL(cudaMemcpyToSymbolAsync, id, info, kind, count)
                CAST_TO_RUNTIME_MEMCPY_TYPE_AND_CALL(cudaMemcpyFromSymbolAsync, id, info, kind, count)
        }
        //driver API
        else
        {
            if (id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD_v2)
            {
                kind = CUPTI_ACTIVITY_MEMCPY_KIND_HTOD;
                count = ((cuMemcpyHtoD_v2_params *) info->functionParams)->ByteCount;
            }
            else if (id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoDAsync_v2)
            {
                kind = CUPTI_ACTIVITY_MEMCPY_KIND_HTOD;
                count = ((cuMemcpyHtoDAsync_v2_params *) info->functionParams)->ByteCount;
            }
            else if (id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH_v2)
            {
                kind = CUPTI_ACTIVITY_MEMCPY_KIND_DTOH;
                count = ((cuMemcpyDtoH_v2_params *) info->functionParams)->ByteCount;
            }
            else if (id == CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoHAsync_v2)
            {
                kind = CUPTI_ACTIVITY_MEMCPY_KIND_DTOH;
                count = ((cuMemcpyDtoHAsync_v2_params *) info->functionParams)->ByteCount;
            }
            else
            {
                //cannot find byte count
                kind = -1;
                count = 0;
            }

        }
    }
    int getMemcpyType(int kind)
    {
        switch(kind)
        {
            case CUPTI_ACTIVITY_MEMCPY_KIND_HTOD:
                return MemcpyHtoD;
            case CUPTI_ACTIVITY_MEMCPY_KIND_DTOH:
                return MemcpyDtoH;
                /*
                   case CUPTI_ACTIVITY_MEMCPY_KIND_HTOA:
                   return MemcpyHtoD;
                   case CUPTI_ACTIVITY_MEMCPY_KIND_ATOH:
                   return MemcpyDtoH;
                   case CUPTI_ACTIVITY_MEMCPY_KIND_ATOA:
                   return MemcpyDtoD;
                   case CUPTI_ACTIVITY_MEMCPY_KIND_ATOD:
                   return MemcpyDtoD;
                   case CUPTI_ACTIVITY_MEMCPY_KIND_DTOA:
                   return MemcpyDtoD;
                   */
            case CUPTI_ACTIVITY_MEMCPY_KIND_DTOD:
                return MemcpyDtoD;
            default:
                return MemcpyUnknown;
        }
    }

#if CUDA_VERSION >= 6000
    int getUnifmemType(int kind)
    {
        switch(kind)
        {
            case CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_BYTES_TRANSFER_HTOD:
                return BytesHtoD;
            case CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_BYTES_TRANSFER_DTOH:
                return BytesDtoH;
            case CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_CPU_PAGE_FAULT_COUNT:
                return CPUPageFault;
            default:
                return UnifmemUnknown;
        }
    }
    static const char *
        getUvmCounterKindString(CUpti_ActivityUnifiedMemoryCounterKind kind)
        {
            switch (kind) 
            {
                case CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_BYTES_TRANSFER_HTOD:
                    return "BYTES_TRANSFER_HTOD";
                case CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_BYTES_TRANSFER_DTOH:
                    return "BYTES_TRANSFER_DTOH";
                case CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_CPU_PAGE_FAULT_COUNT:
                    return "CPU_PAGE_FAULT_COUNT";
                default:
                    break;
            }
            return "<unknown>";
        }

    static const char *
        getUvmCounterScopeString(CUpti_ActivityUnifiedMemoryCounterScope scope)
        {
            switch (scope) 
            {
                case CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_SCOPE_PROCESS_SINGLE_DEVICE:
                    return "PROCESS_SINGLE_DEVICE";
                case CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_SCOPE_PROCESS_ALL_DEVICES:
                    return "PROCESS_ALL_DEVICES";
                default:
                    break;
            }
            return "<unknown>";
        }
#endif

    const char *demangleName(const char* name)
    {
        const char *dem_name = 0;
        //printf("demangling: %s.\n", name);
#if defined(HAVE_GNU_DEMANGLE) && HAVE_GNU_DEMANGLE
        //printf("demangling name....\n");
        dem_name = cplus_demangle(name, DMGL_PARAMS | DMGL_ANSI | DMGL_VERBOSE |
                DMGL_TYPES);
        //check to see if demangling failed (name was not mangled).
        if (dem_name == NULL)
        {
            dem_name = name;
        }
#else
        dem_name = name;
#endif /* HAVE_GPU_DEMANGLE */
        //printf("demanged: %s.\n", dem_name);
        return dem_name;
    }


    bool cupti_api_runtime()
    {
        return (0 == strcasecmp(TauEnv_get_cupti_api(), "runtime") || 
                0 == strcasecmp(TauEnv_get_cupti_api(), "both"));
    }
    bool cupti_api_driver()
    {
        return (0 == strcasecmp(TauEnv_get_cupti_api(), "driver") || 
                0 == strcasecmp(TauEnv_get_cupti_api(), "both")); 
    }

    int get_device_count()
    {
#if CUDA_VERSION >= 5000
        int device_count;
        cuDeviceGetCount(&device_count);
        return device_count;
#else
        return 1;
#endif

    }

    int get_device_id() 
    {
        int deviceId;
        cudaGetDevice(&deviceId);
        return deviceId;
    }

    FILE* createFileSourceSass(int task_id) 
    {
        char str_int[5];
        sprintf (str_int, "%d", (task_id));
        if ( fp_source[task_id] == NULL ) {
#ifdef TAU_DEBUG_CUPTI_SASS
            printf("About to create file pointer for source csv: %i\n", task_id);
#endif
            char str_source[500];
            strcpy (str_source,TauEnv_get_profiledir());
            strcat (str_source,"/");
            strcat (str_source,"sass_source_");
            strcat (str_source, str_int);
            strcat (str_source, ".csv");

            fp_source[task_id] = fopen(str_source, "a");
            fprintf(fp_source[task_id], "timestamp,id,fileName,lineNumber,kind\n");
            if (fp_source[task_id] == NULL) {
#ifdef TAU_DEBUG_CUPTI_SASS
                printf("fp_source[%i] failed\n", task_id);
#endif
            }
            else {
#ifdef TAU_DEBUG_CUPTI_SASS
                printf("fp_source[%i] created successfully\n", task_id);
#endif
            }
        }
        else {
#ifdef TAU_DEBUG_CUPTI_SASS
            printf("fp_source[%i] already exists!\n", task_id);
#endif
        }
        return fp_source[task_id];
    }

    FILE* createFileInstrSass(int task_id) 
    {
        char str_int[5];
        sprintf (str_int, "%d", (task_id));
        if ( fp_instr[task_id] == NULL ) {
#ifdef TAU_DEBUG_CUPTI_SASS
            printf("About to create file pointer for instr csv: %i\n", task_id);
#endif
            char str_instr[500];
            strcpy (str_instr, TauEnv_get_profiledir());
            strcat (str_instr,"/");
            strcat (str_instr,"sass_instr_");
            strcat (str_instr, str_int);
            strcat (str_instr, ".csv");

            fp_instr[task_id] = fopen(str_instr, "a");
            fprintf(fp_instr[task_id], "timestamp,correlationId,executed,flags,functionId,kind,\
                    notPredOffThreadsExecuted,pcOffset,sourceLocatorId,threadsExecuted\n");
            if (fp_instr[task_id] == NULL) {
#ifdef TAU_DEBUG_CUPTI_SASS
                printf("fp_instr[%i] failed\n", task_id);
#endif
            }
            else {
#ifdef TAU_DEBUG_CUPTI_SASS
                printf("fp_instr[%i] created successfully\n", task_id);
#endif
            }
        }
        else {
#ifdef TAU_DEBUG_CUPTI_SASS
            printf("fp_instr[%i] already exists!\n", task_id);
#endif
        }
        return fp_instr[task_id];
    }

    FILE* createFileFuncSass(int task_id) 
    {
        char str_int[5];
        sprintf (str_int, "%d", (task_id));
        if ( fp_func[task_id] == NULL ) {
#ifdef TAU_DEBUG_CUPTI_SASS
            printf("About to create file pointer for func csv: %i\n", task_id);
#endif
            char str_func[500];
            strcpy (str_func, TauEnv_get_profiledir());
            strcat (str_func,"/");
            strcat (str_func,"sass_func_");
            strcat (str_func, str_int);
            strcat (str_func, ".csv");

            fp_func[task_id] = fopen(str_func, "a");
            fprintf(fp_func[task_id], "timestamp;contextId;functionIndex;id;kind;moduleId;name;demangled\n");
            if (fp_func[task_id] == NULL) {
#ifdef TAU_DEBUG_CUPTI_SASS
                printf("fp_func[%i] failed\n", task_id);
#endif
            }
            else {
#ifdef TAU_DEBUG_CUPTI_SASS
                printf("fp_func[%i] created successfully\n", task_id);
#endif
            }
        }
        else {
#ifdef TAU_DEBUG_CUPTI_SASS
            printf("fp_func[%i] already exists!\n", task_id);
#endif
        }
        return fp_func[task_id];
    }

    void record_gpu_counters_at_launch(int device, int task)
    { 
        kernels_encountered[task]++;
        if (Tau_CuptiLayer_get_num_events() > 0 &&
                !counters_averaged_warning_issued[task] && 
                kernels_encountered[task] > 1) {
            TAU_VERBOSE("TAU Warning: CUPTI events will be avereged, multiple kernel deteched between synchronization points.\n");
            counters_averaged_warning_issued[task] = true;
            for (int n = 0; n < Tau_CuptiLayer_get_num_events(); n++) {
                Tau_CuptiLayer_set_event_name(n, TAU_CUPTI_COUNTER_AVERAGED); 
            }
        }
        int n_counters = Tau_CuptiLayer_get_num_events();
        if (n_counters > 0 && counters_at_last_launch[task][0] == ULONG_MAX) {
            Tau_CuptiLayer_read_counters(device, task, counters_at_last_launch[task]);
        }
#ifdef TAU_CUPTI_DEBUG_COUNTERS
        std::cout << "at launch (" << device << ") ====> " << std::endl;
        for (int n = 0; n < Tau_CuptiLayer_get_num_events(); n++) {
            std::cout << "\tlast launch:      " << counters_at_last_launch[task][n] << std::endl;
            std::cout << "\tcurrent counters: " << current_counters[task][n] << std::endl;
        }
#endif
    }

    void record_gpu_counters_at_sync(int device, int task)
    {
        if (kernels_encountered[task] == 0) {
            return;
        }
        Tau_CuptiLayer_read_counters(device, task, current_counters[task]);
#ifdef TAU_CUPTI_DEBUG_COUNTERS
        std::cout << "at sync (" << device << ") ====> " << std::endl;
        for (int n = 0; n < Tau_CuptiLayer_get_num_events(); n++) {
            std::cout << "\tlast launch:      " << counters_at_last_launch[task][n] << std::endl;
            std::cout << "\tcurrent counters: " << current_counters[task][n] << std::endl;
        }
#endif
    }

    void clear_counters(int device)
    {
        for (int n = 0; n < Tau_CuptiLayer_get_num_events(); n++)
        {
            counters_at_last_launch[device][n] = ULONG_MAX;
        }
        kernels_encountered[device] = 0;
        kernels_recorded[device] = 0;

    }
    int get_device_from_id(int id) {
        return (correlDeviceMap.find(id) != correlDeviceMap.end()) ? correlDeviceMap[id] : 0;
    }

    void record_gpu_counters_at_sync() {
        int count_iter;
        for(int i=0; i<deviceContextThreadVector.size(); i++) {
            record_gpu_counters_at_sync(i, deviceContextThreadVector[i]);
        }
    }

#endif //CUPTI API VERSION >= 2
