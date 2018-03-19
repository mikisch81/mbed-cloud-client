/*******************************************************************************
 * Copyright 2016, 2017 ARM Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/
#define _GNU_SOURCE // This is for ppoll found in poll.h
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <mqueue.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>

#include "pal.h"
#include "pal_plat_rtos.h"

 /*
 * The realtime clock is in nano seconds resolution. This is too much for us, so we use "longer" ticks.
 * Below are relevant defines.
 * make sure they all coherent. Can use one at the other, but will add some unneeded calculations.
 */
#define NANOS_PER_TICK 100
#define TICKS_PER_MICRO  10L
#define TICKS_PER_MILLI  TICKS_PER_MICRO * 1000
#define TICKS_PER_SECOND TICKS_PER_MILLI * 1000

// priorities must be positive, so shift all by this margin. we might want to do smarter convert.
#define LINUX_THREAD_PRIORITY_BASE 10

//  message Queues names related staff:
#define MQ_FILENAME_LEN 10

#ifndef CLOCK_MONOTONIC_RAW //a workaround for the operWRT port that missing this include
#define CLOCK_MONOTONIC_RAW 4 //http://elixir.free-electrons.com/linux/latest/source/include/uapi/linux/time.h
#endif

PAL_PRIVATE char g_mqName[MQ_FILENAME_LEN];
PAL_PRIVATE int g_mqNextNameNum = 0;

PAL_PRIVATE int16_t g_threadPriorityMap[PAL_NUMBER_OF_THREAD_PRIORITIES] = 
{ 
    7,  // PAL_osPriorityIdle
    8,  // PAL_osPriorityLow
    9,  // PAL_osPriorityReservedTRNG
    10, // PAL_osPriorityBelowNormal
    11, // PAL_osPriorityNormal
    12, // PAL_osPriorityAboveNormal
    13, // PAL_osPriorityReservedDNS
    14, // PAL_osPriorityReservedSockets
    15, // PAL_osPriorityHigh
    16, // PAL_osPriorityReservedHighResTimer
    17  // PAL_osPriorityRealtime
};

extern palStatus_t pal_plat_getRandomBufferFromHW(uint8_t *randomBuf, size_t bufSizeBytes, size_t* actualRandomSizeBytes);

inline PAL_PRIVATE void nextMessageQName()
{
    g_mqNextNameNum++;
    for (int j = 4, divider = 10000; j < 9; j++, divider /= 10)
    {
        g_mqName[j] = '0' + (g_mqNextNameNum / divider) %10 ; //just to make sure we don't write more then 1 digit.
    }
    g_mqName[9] = '\0';
}


/*! Initiate a system reboot.
 */
void pal_plat_osReboot(void)
{
    // Reboot the device
    reboot(RB_AUTOBOOT);
}

/*! Initialize all data structures (semaphores, mutexs, memory pools, message queues) at system initialization.
*	In case of a failure in any of the initializations, the function returns with an error and stops the rest of the initializations.
* @param[in] opaqueContext The context passed to the initialization (not required for generic CMSIS, pass NULL in this case).
* \return PAL_SUCCESS(0) in case of success, PAL_ERR_CREATION_FAILED in case of failure.
*/
palStatus_t pal_plat_RTOSInitialize(void* opaqueContext)
{
    palStatus_t status = PAL_SUCCESS;
    (void)opaqueContext;
    strncpy(g_mqName, "/pal00001", MQ_FILENAME_LEN);
    g_mqNextNameNum = 1;   // used for the next name
#if (PAL_USE_HW_RTC)
    status = pal_plat_rtcInit();
#endif
    return status;
}

/*! De-Initialize thread objects.
 */
palStatus_t pal_plat_RTOSDestroy(void)
{
    palStatus_t ret = PAL_SUCCESS;
#if PAL_USE_HW_RTC
    ret = pal_plat_rtcDeInit();
#endif
	return ret;
}

/*return The RTOS kernel system timer counter, in microseconds
 */

uint64_t pal_plat_osKernelSysTick(void) // optional API - not part of original CMSIS API.
{
    /*Using clock_gettime is more accurate, but then we have to convert it to ticks. we are using a tick every 100 nanoseconds*/
    struct timespec ts;
    uint64_t ticks;
    //TODO: error handling
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

    ticks = (uint64_t) (ts.tv_sec * (uint64_t)TICKS_PER_SECOND
            + (ts.tv_nsec / NANOS_PER_TICK));
    return ticks;
}

/* Convert the value from microseconds to kernel sys ticks.
 * This is the same as CMSIS macro osKernelSysTickMicroSec.
 * since we return microsecods as ticks, just return the value
 */
uint64_t pal_plat_osKernelSysTickMicroSec(uint64_t microseconds)
{

    //convert to nanoseconds
    return microseconds * TICKS_PER_MICRO;
}

/*! Get the system tick frequency.
 * \return The system tick frequency.
 */
inline uint64_t pal_plat_osKernelSysTickFrequency(void)
{
    /* since we use clock_gettime, with resolution of 100 nanosecond per tick*/
    return TICKS_PER_SECOND;
}

void* threadFunction(void* arg)
{
    palThreadServiceBridge_t* bridge = (palThreadServiceBridge_t*)arg;
    bridge->function(bridge->threadData);
    return NULL;
}

int16_t pal_plat_osThreadTranslatePriority(palThreadPriority_t priority)
{
    return g_threadPriorityMap[priority];
}

palStatus_t pal_plat_osThreadDataInitialize(palThreadPortData* portData, int16_t priority, uint32_t stackSize)
{
    return PAL_SUCCESS;
}

palStatus_t pal_plat_osThreadRun(palThreadServiceBridge_t* bridge, palThreadID_t* osThreadID)
{
    palStatus_t status = PAL_SUCCESS;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int err = pthread_attr_setstacksize(&attr, bridge->threadData->stackSize);
    if (0 != err)
    {
        status = PAL_ERR_GENERIC_FAILURE;
        goto finish;
    }
    if (0 != pthread_attr_setschedpolicy(&attr, SCHED_RR))
    {
        status = PAL_ERR_GENERIC_FAILURE;
        goto finish;
    }
#if (PAL_SIMULATOR_TEST_ENABLE == 0) //Disable ONLY for Linux PC simulator 
    if (0 != pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED))
    {
        status = PAL_ERR_GENERIC_FAILURE;
        goto finish;
    }
#endif    
    if (0 != pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))
    {
        status = PAL_ERR_GENERIC_FAILURE;
        goto finish;
    }

    struct sched_param schedParam;
    schedParam.sched_priority = bridge->threadData->osPriority;
    if (0 != pthread_attr_setschedparam(&attr, &schedParam))
    {
        status = PAL_ERR_GENERIC_FAILURE;
        goto finish;
    }

    pthread_t threadID = (pthread_t)NULL;
    int retVal = pthread_create(&threadID, &attr, threadFunction, (void*)bridge);
    pthread_attr_destroy(&attr); // destroy the thread attributes object since it's no longer needed
    if (0 != retVal)
    {
        if (EPERM == retVal)
        {
            status = PAL_ERR_RTOS_PRIORITY;
        }
        else
        {
            status = PAL_ERR_RTOS_RESOURCE;
        }
        goto finish;
    }

    if (((palThreadID_t)PAL_INVALID_THREAD == threadID) || (0 == threadID))
    {
        status = PAL_ERR_GENERIC_FAILURE;
    }
    else
    {
        *osThreadID = (palThreadID_t)threadID;
    }

finish:
    return status;
}

palStatus_t pal_plat_osThreadDataCleanup(palThreadData_t* threadData)
{
    return PAL_SUCCESS;
}

palStatus_t pal_plat_osThreadTerminate(palThreadData_t* threadData)
{
    palStatus_t status = PAL_ERR_RTOS_TASK;
    int osStatus = 0;
    pthread_t threadID = (pthread_t)(threadData->osThreadID);
    if (pthread_self() != threadID) // terminate only if not trying to terminate from self
    {
        osStatus = pthread_cancel(threadID);
        if ((0 == osStatus) || (ESRCH == osStatus))
        {
            status = PAL_SUCCESS;
        }
        else
        {
            status = PAL_ERR_RTOS_RESOURCE;
        }
    }
    return status;
}

palThreadID_t pal_plat_osThreadGetId(void)
{
    palThreadID_t osThreadID = (palThreadID_t)pthread_self();
    return osThreadID;
}

/*! Wait for a specified period of time in milliseconds.
 *
 * @param[in] milliseconds The number of milliseconds to wait before proceeding.
 *
 * \return The status in the form of palStatus_t; PAL_SUCCESS(0) in case of success, a negative value indicating a specific error code in case of failure.
 */
palStatus_t pal_plat_osDelay(uint32_t milliseconds)
{
    struct timespec sTime;
    struct timespec rTime; // this will return how much sleep time still left in case of interrupted sleep
    int stat;
    //init rTime, as we will copy it over to stime inside the do-while loop.
    rTime.tv_sec = milliseconds / 1000;
    rTime.tv_nsec = PAL_MILLI_TO_NANO(milliseconds);

    do
    {
        sTime.tv_sec = rTime.tv_sec;
        sTime.tv_nsec = rTime.tv_nsec;
        stat = nanosleep(&sTime, &rTime);
    } while ((-1 == stat) && (EINTR ==errno)) ;
    return (stat == 0) ? PAL_SUCCESS : PAL_ERR_GENERIC_FAILURE;
}

/*
 * Internal struct to handle timers.
 */

struct palTimerInfo
{
    timer_t handle;
    palTimerFuncPtr function;
    void *funcArgs;
    palTimerType_t timerType;
    bool isHighRes;
};

/*
 * internal function used to handle timers expiration events.
 */
PAL_PRIVATE void palTimerEventHandler(void* args)
{
    struct palTimerInfo* timer = (struct palTimerInfo *) args;

    if (NULL == timer)
    { // no timer anymore, so just return.
        return;
    }

    //call the callback function
    timer->function(timer->funcArgs);
}


/*
* Internal struct to handle timers.
*/

#define PAL_HIGH_RES_TIMER_THRESHOLD_MS 100

typedef struct palHighResTimerThreadContext
{
    palTimerFuncPtr function;
    void *funcArgs;
    uint32_t intervalMS;
} palHighResTimerThreadContext_t;


static palThreadID_t s_palHighResTimerThreadID = NULLPTR;
static bool s_palHighResTimerThreadInUse =  0;
static palHighResTimerThreadContext_t s_palHighResTimerThreadContext = {0};

/*
*  callback for handling high precision timer callbacks (currently only one is supported)
*/

PAL_PRIVATE void palHighResTimerThread(void const *args)
{
    palHighResTimerThreadContext_t* context = (palHighResTimerThreadContext_t*)args;
    uint32_t timer_period_ms = context->intervalMS;
    int err = 0;
    struct timespec next_timeout_ts;
    err = clock_gettime(CLOCK_MONOTONIC, &next_timeout_ts);
    assert(err == 0);

    while(1) {
        // Determine absolute time we want to sleep until
        next_timeout_ts.tv_nsec += PAL_NANO_PER_MILLI * timer_period_ms;
        if (next_timeout_ts.tv_nsec >= PAL_NANO_PER_SECOND) 
        {
            next_timeout_ts.tv_nsec = next_timeout_ts.tv_nsec - PAL_NANO_PER_SECOND;
            next_timeout_ts.tv_sec += 1;
        }

        // Call nanosleep until error or no interrupt, ie. return code is 0
        do {
            err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_timeout_ts, NULL);
            assert(err == 0 || err == EINTR);
        } while(err == EINTR);

        // Done sleeping, call callback
        context->function(context->funcArgs);
    }
}

PAL_PRIVATE palStatus_t startHighResTimerThread(palTimerFuncPtr function, void *funcArgs , uint32_t intervalMS)
{
    s_palHighResTimerThreadContext.function = function;
    s_palHighResTimerThreadContext.funcArgs = funcArgs;
    s_palHighResTimerThreadContext.intervalMS = intervalMS;
    palStatus_t status = pal_osThreadCreateWithAlloc(palHighResTimerThread, &s_palHighResTimerThreadContext, PAL_osPriorityReservedHighResTimer,
        PAL_RTOS_HIGH_RES_TIMER_THREAD_STACK_SIZE, NULL, &s_palHighResTimerThreadID);
    return status;
}


/*! Create a timer.
 *
 * @param[in] function A function pointer to the timer callback function.
 * @param[in] funcArgument An argument for the timer callback function.
 * @param[in] timerType The timer type to be created, periodic or oneShot.
 * @param[out] timerID The ID of the created timer, zero value indicates an error.
 *
 * \return PAL_SUCCESS when the timer was created successfully. A specific error in case of failure.
 */
palStatus_t pal_plat_osTimerCreate(palTimerFuncPtr function, void* funcArgument,
        palTimerType_t timerType, palTimerID_t* timerID)
{

    palStatus_t status = PAL_SUCCESS;
    struct palTimerInfo* timerInfo = NULL;
    {
        struct sigevent sig;
        timer_t localTimer;

        if ((NULL == timerID) || (NULL == (void*) function))
        {
            return PAL_ERR_INVALID_ARGUMENT;
        }

        timerInfo = (struct palTimerInfo*) malloc(sizeof(struct palTimerInfo));
        if (NULL == timerInfo)
        {
            status = PAL_ERR_NO_MEMORY;
            goto finish;
        }

        timerInfo->function = function;
        timerInfo->funcArgs = funcArgument;
        timerInfo->timerType = timerType;
        timerInfo->isHighRes = false;

        memset(&sig, 0, sizeof(sig));

        sig.sigev_notify = SIGEV_THREAD;
        sig.sigev_signo = 0;
        sig.sigev_value.sival_ptr = timerInfo;
        sig.sigev_notify_function = (void (*)(union sigval)) palTimerEventHandler;

        int ret = timer_create(CLOCK_MONOTONIC, &sig, &localTimer);
        if (-1 == ret)
        {
            if (EINVAL == errno)
            {
                status = PAL_ERR_INVALID_ARGUMENT;
                goto finish;
            }
            if (ENOMEM == errno)
            {
                status = PAL_ERR_NO_MEMORY;
                goto finish;
            }
            PAL_LOG(ERR, "Rtos timer create error %d", ret);
            status = PAL_ERR_GENERIC_FAILURE;
            goto finish;
        }

        // managed to create the timer - finish up
        timerInfo->handle = localTimer;
        *timerID = (palTimerID_t) timerInfo;
    }
    finish: if (PAL_SUCCESS != status)
    {
        if (NULL != timerInfo)
        {
            free(timerInfo);
            *timerID = (palTimerID_t) NULL;
        }
    }
    return status;
}

/* Convert milliseconds into seconds and nanoseconds inside a timespec struct
 */
PAL_PRIVATE void convertMilli2Timespec(uint32_t millisec, struct timespec* ts)
{
    ts->tv_sec = millisec / 1000;
    ts->tv_nsec = PAL_MILLI_TO_NANO(millisec);
}

/*! Start or restart a timer.
 *
 * @param[in] timerID The handle for the timer to start.
 * @param[in] millisec The time in milliseconds to set the timer to.
 *
 * \return The status in the form of palStatus_t; PAL_SUCCESS(0) in case of success, a negative value indicating a specific error code in case of failure.
 */
palStatus_t pal_plat_osTimerStart(palTimerID_t timerID, uint32_t millisec)
{
    palStatus_t status = PAL_SUCCESS;
    if (NULL == (struct palTimerInfo *) timerID)
    {
        return PAL_ERR_INVALID_ARGUMENT;
    }

    struct palTimerInfo* timerInfo = (struct palTimerInfo *) timerID;
    struct itimerspec its;


    if ((millisec <= PAL_HIGH_RES_TIMER_THRESHOLD_MS) && (palOsTimerPeriodic == timerInfo->timerType )) // periodic high res timer  - we only support 1 (workaround for issue when lots of threads are created in linux)
    {
        if (true == s_palHighResTimerThreadInUse)
        {
            status = PAL_ERR_NO_HIGH_RES_TIMER_LEFT;
        }
        else
        {
            status = startHighResTimerThread(timerInfo->function, timerInfo->funcArgs, millisec);
            if (PAL_SUCCESS == status)
            {
                timerInfo->isHighRes = true;
                s_palHighResTimerThreadInUse = true;
            }
        }
    }
    else // otherwise handle normally
    {
        convertMilli2Timespec(millisec, &(its.it_value));

        if (palOsTimerPeriodic == timerInfo->timerType)
        {
            convertMilli2Timespec(millisec, &(its.it_interval));
        }
        else
        {  // one time timer
            convertMilli2Timespec(0, &(its.it_interval));
        }

        if (-1 == timer_settime(timerInfo->handle, 0, &its, NULL))
        {
            status = PAL_ERR_INVALID_ARGUMENT;
        }
    }

    return status;
}

/*! Stop a timer.
 *
 * @param[in] timerID The handle for the timer to stop.
 *
 * \return The status in the form of palStatus_t; PAL_SUCCESS(0) in case of success, a negative value indicating a specific error code in case of failure.
 */
palStatus_t pal_plat_osTimerStop(palTimerID_t timerID)
{
    palStatus_t status = PAL_SUCCESS;
    if (NULL == (struct palTimerInfo *) timerID)
    {
        return PAL_ERR_INVALID_ARGUMENT;
    }

    struct palTimerInfo* timerInfo = (struct palTimerInfo *) timerID;
    struct itimerspec its;

    if ((true == timerInfo->isHighRes) && (0 != s_palHighResTimerThreadInUse )) // if  high res timer clean up thread.
    {
        status = pal_osThreadTerminate(&s_palHighResTimerThreadID);
        if (PAL_SUCCESS == status)
        {
            timerInfo->isHighRes = false;
            s_palHighResTimerThreadInUse = false;
        }
    }
    else // otherwise process normally
    {
        // set timer to 0 to disarm it.
        convertMilli2Timespec(0, &(its.it_value));

        convertMilli2Timespec(0, &(its.it_interval));

        if (-1 == timer_settime(timerInfo->handle, 0, &its, NULL))
        {
            status = PAL_ERR_INVALID_ARGUMENT;
        }
    }

    return status;
}

/*! Delete the timer object
 *
 * @param[inout] timerID The handle for the timer to delete. In success, *timerID = NULL.
 *
 * \return PAL_SUCCESS when the timer was deleted successfully, PAL_ERR_RTOS_PARAMETER when the timerID is incorrect.
 */
palStatus_t pal_plat_osTimerDelete(palTimerID_t* timerID)
{
    palStatus_t status = PAL_SUCCESS, tempStatus;
    if (NULL == timerID)
    {
        return PAL_ERR_INVALID_ARGUMENT;
    }
    struct palTimerInfo* timerInfo = (struct palTimerInfo *) *timerID;
    if (NULL == timerInfo)
    {
        status = PAL_ERR_RTOS_PARAMETER;
    }

    if ((PAL_SUCCESS == status) && (true == timerInfo->isHighRes) && (0 != s_palHighResTimerThreadInUse)) //  if high res timer delted before stopping => clean up thread.
    {
        tempStatus = pal_osThreadTerminate(&s_palHighResTimerThreadID);
        if (PAL_SUCCESS == tempStatus)
        {
            timerInfo->isHighRes = false;
            s_palHighResTimerThreadInUse = false;
        }
        else
        {
            status = tempStatus;
        }
    }

    if (PAL_SUCCESS == status)
    {
        timer_t lt = timerInfo->handle;
        if (-1 == timer_delete(lt))
        {
            status = PAL_ERR_RTOS_RESOURCE;
        }

        free(timerInfo);
        *timerID = (palTimerID_t) NULL;
    }
    return status;
}

/*! Create and initialize a mutex object.
 *
 * @param[out] mutexID The created mutex ID handle, zero value indicates an error.
 *
 * \return PAL_SUCCESS when the mutex was created successfully, a specific error in case of failure.
 */
palStatus_t pal_plat_osMutexCreate(palMutexID_t* mutexID)
{
    palStatus_t status = PAL_SUCCESS;
    pthread_mutex_t* mutex = NULL;
    {
        int ret;
        if (NULL == mutexID)
        {
            return PAL_ERR_INVALID_ARGUMENT;
        }

        mutex = malloc(sizeof(pthread_mutex_t));
        if (NULL == mutex)
        {
            status = PAL_ERR_NO_MEMORY;
            goto finish;
        }

        pthread_mutexattr_t mutexAttr;
        pthread_mutexattr_init(&mutexAttr);
        pthread_mutexattr_settype(&mutexAttr, PTHREAD_MUTEX_RECURSIVE);
        ret = pthread_mutex_init(mutex, &mutexAttr);

        if (0 != ret)
        {
            if (ENOMEM == ret)
            {
                status = PAL_ERR_NO_MEMORY;
            }
            else
            {
                PAL_LOG(ERR, "Rtos mutex create status %d", ret);
                status = PAL_ERR_GENERIC_FAILURE;
            }
            goto finish;
        }
        *mutexID = (palMutexID_t) mutex;
    }
    finish: if (PAL_SUCCESS != status)
    {
        if (NULL != mutex)
        {
            free(mutex);
        }
    }
    return status;
}

/* Wait until a mutex becomes available.
 *
 * @param[in] mutexID The handle for the mutex.
 * @param[in] millisec The timeout for the waiting operation if the timeout expires before the semaphore is released and an error is returned from the function.
 *
 * \return The status in the form of palStatus_t; PAL_SUCCESS(0) in case of success, one of the following error codes in case of failure:
 * 		  PAL_ERR_RTOS_RESOURCE - Mutex not available but no timeout set.
 * 		  PAL_ERR_RTOS_TIMEOUT - Mutex was not available until timeout expired.
 * 		  PAL_ERR_RTOS_PARAMETER - Mutex ID is invalid.
 * 		  PAL_ERR_RTOS_ISR - Cannot be called from interrupt service routines.
 */
palStatus_t pal_plat_osMutexWait(palMutexID_t mutexID, uint32_t millisec)
{
    palStatus_t status = PAL_SUCCESS;
    int err;
    if (NULL == ((pthread_mutex_t*) mutexID))
    {
        return PAL_ERR_INVALID_ARGUMENT;
    }
    pthread_mutex_t* mutex = (pthread_mutex_t*) mutexID;

    if (PAL_RTOS_WAIT_FOREVER != millisec)
    {
        /* calculate the wait absolute time */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);

        ts.tv_sec += (millisec / PAL_MILLI_PER_SECOND);
        ts.tv_nsec += PAL_MILLI_TO_NANO(millisec);
        ts.tv_sec += ts.tv_nsec / PAL_NANO_PER_SECOND; // if there is some overflow in the addition of nanoseconds.
        ts.tv_nsec = ts.tv_nsec % PAL_NANO_PER_SECOND;

        while ((err = pthread_mutex_timedlock(mutex, &ts)) != 0 && err == EINTR)
        {
            continue; /* Restart if interrupted by handler */
        }
    }
    else
    { // wait for ever
        err = pthread_mutex_lock(mutex);
    }

    if (0 != err)
    {
        if (err == ETIMEDOUT)
        {
            status = PAL_ERR_RTOS_TIMEOUT;
        }
        else
        {
            PAL_LOG(ERR, "Rtos mutex wait status %d", err);
            status = PAL_ERR_GENERIC_FAILURE;
        }
    }

    return status;
}

/* Release a mutex that was obtained by osMutexWait.
 *
 * @param[in] mutexID The handle for the mutex.
 *
 * \return The status in the form of palStatus_t; PAL_SUCCESS(0) in case of success, a negative value indicating a specific error code in case of failure.
 */
palStatus_t pal_plat_osMutexRelease(palMutexID_t mutexID)
{
    palStatus_t status = PAL_SUCCESS;
    int result = 0;

    pthread_mutex_t* mutex = (pthread_mutex_t*) mutexID;
    if (NULL == mutex)
    {
        return PAL_ERR_INVALID_ARGUMENT;
    }

    result = pthread_mutex_unlock(mutex);
    if (0 != result)
    {
        // only reason this might fail - process don't have permission for mutex.
        PAL_LOG(ERR, "Rtos mutex release failure - %d",result);
        status = PAL_ERR_GENERIC_FAILURE;
    }
    return status;
}

/*Delete a mutex object.
 *
 * @param[inout] mutexID The ID of the mutex to delete. In success, *mutexID = NULL.
 *
 * \return PAL_SUCCESS when the mutex was deleted successfully, one of the following error codes in case of failure:
 * 		  PAL_ERR_RTOS_RESOURCE - Mutex already released.
 * 		  PAL_ERR_RTOS_PARAMETER - Mutex ID is invalid.
 * 		  PAL_ERR_RTOS_ISR - Cannot be called from interrupt service routines.
 * \note After this call, mutex_id is no longer valid and cannot be used.
 */
palStatus_t pal_plat_osMutexDelete(palMutexID_t* mutexID)
{
    palStatus_t status = PAL_SUCCESS;
    uint32_t ret;
    if (NULL == mutexID)
    {
        return PAL_ERR_INVALID_ARGUMENT;
    }
    pthread_mutex_t* mutex = (pthread_mutex_t*) *mutexID;

    if (NULL == mutex)
    {
        status = PAL_ERR_RTOS_RESOURCE;
    }
    ret = pthread_mutex_destroy(mutex);
    if ((PAL_SUCCESS == status) && (0 != ret))
    {
        PAL_LOG(ERR,"pal_plat_osMutexDelete 0x%x",ret);
        status = PAL_ERR_RTOS_RESOURCE;
    }
    if (NULL != mutex)
    {
        free(mutex);
    }

    *mutexID = (palMutexID_t) NULL;
    return status;
}

/* Create and initialize a semaphore object.
 *
 * Semaphore is shared between threads, but not process.
 *
 * @param[in] count The number of available resources.
 * @param[out] semaphoreID The ID of the created semaphore, zero value indicates an error.
 *
 * \return PAL_SUCCESS when the semaphore was created successfully, a specific error in case of failure.
 */
palStatus_t pal_plat_osSemaphoreCreate(uint32_t count,
        palSemaphoreID_t* semaphoreID)
{
    palStatus_t status = PAL_SUCCESS;
    sem_t* semaphore = NULL;

    {
        if (NULL == semaphoreID)
        {
            return PAL_ERR_INVALID_ARGUMENT;
        }
        semaphore = malloc(sizeof(sem_t));
        if (NULL == semaphore)
        {
            status = PAL_ERR_NO_MEMORY;
            goto finish;
        }
        /* create the semaphore as shared between threads */
        int ret = sem_init(semaphore, 0, count);
        if (-1 == ret)
        {
            if (EINVAL == errno)
            {
                /* count is too big */
                status = PAL_ERR_INVALID_ARGUMENT;
            }
            else
            {
                PAL_LOG(ERR, "Rtos semaphore init error %d", ret);
                status = PAL_ERR_GENERIC_FAILURE;
            }
            goto finish;
        }

        *semaphoreID = (palSemaphoreID_t) semaphore;
    }
    finish: if (PAL_SUCCESS != status)
    {
        if (NULL != semaphore)
        {
            free(semaphore);
        }
        *semaphoreID = (palSemaphoreID_t) NULL;
    }
    return status;
}

/* Wait until a semaphore token becomes available.
 *
 * @param[in] semaphoreID The handle for the semaphore.
 * @param[in] millisec The timeout for the waiting operation if the timeout expires before the semaphore is released and an error is returned from the function.
 * @param[out] countersAvailable The number of semaphores available (before the wait), if semaphores are not available (timeout/error) zero is returned.
 *
 * \return The status in the form of palStatus_t; PAL_SUCCESS(0) in case of success, one of the following error codes in case of failure:
 * 		PAL_ERR_RTOS_TIMEOUT - Semaphore was not available until timeout expired.
 *	    PAL_ERR_RTOS_PARAMETER - Semaphore ID is invalid.
 *	    PAL_ERR_INVALID_ARGUMENT - countersAvailable is NULL
 *
 *	    NOTES: 1. counterAvailable returns 0 in case there are no semaphores available or there are other threads waiting on it.
 *	              Value is not thread safe - it might be changed by the time it is read/returned.
 *	           2. timed wait is using absolute time.
 */
palStatus_t pal_plat_osSemaphoreWait(palSemaphoreID_t semaphoreID,
        uint32_t millisec, int32_t* countersAvailable)
{
    palStatus_t status = PAL_SUCCESS;
    int tmpCounters = 0;
    {
        int err;
        sem_t* sem = (sem_t*) semaphoreID;
        if ((NULL == sem))
        {
            return PAL_ERR_INVALID_ARGUMENT;
        }

        if (PAL_RTOS_WAIT_FOREVER != millisec)
        {
            /* calculate the wait absolute time */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += millisec / PAL_MILLI_PER_SECOND;
            ts.tv_nsec += PAL_MILLI_TO_NANO(millisec);
            ts.tv_sec += ts.tv_nsec / PAL_NANO_PER_SECOND; // in case there is overflow in the nanoseconds.
            ts.tv_nsec = ts.tv_nsec % PAL_NANO_PER_SECOND;

            while ((err = sem_timedwait(sem, &ts)) == -1 && errno == EINTR)
                continue; /* Restart if interrupted by handler */
        }
        else
        { // wait for ever
            do
            {
                err = sem_wait(sem);

                /* loop again if the wait was interrupted by a signal */
            } while ((err == -1) && (errno == EINTR));
        }

        if (-1 == err)
        {
            tmpCounters = 0;
            if (errno == ETIMEDOUT)
            {
                status = PAL_ERR_RTOS_TIMEOUT;
            }
            else
            { /* seems this is not a valid semaphore */
                status = PAL_ERR_RTOS_PARAMETER;
            }
            goto finish;
        }
        /* get the counter number, shouldn't fail, as we already know this is valid semaphore */
        sem_getvalue(sem, &tmpCounters);
    }
    finish:
    if (NULL != countersAvailable)
    {
        *countersAvailable = tmpCounters;
    }
    return status;
}

/*! Release a semaphore token.
 *
 * @param[in] semaphoreID The handle for the semaphore.
 *
 * \return The status in the form of palStatus_t; PAL_SUCCESS(0) in case of success, a negative value indicating a specific error code in case of failure.
 */
palStatus_t pal_plat_osSemaphoreRelease(palSemaphoreID_t semaphoreID)
{
    palStatus_t status = PAL_SUCCESS;
    sem_t* sem = (sem_t*) semaphoreID;

    if (NULL == sem)
    {
        return PAL_ERR_INVALID_ARGUMENT;
    }

    if (-1 == sem_post(sem))
    {
        if (EINVAL == errno)
        {
            status = PAL_ERR_RTOS_PARAMETER;
        }
        else
        { /* max value of semaphore exeeded */
            PAL_LOG(ERR, "Rtos semaphore release error %d", errno);
            status = PAL_ERR_GENERIC_FAILURE;
        }
    }

    return status;
}

/*! Delete a semaphore object.
 *
 * @param[inout] semaphoreID: The ID of the semaphore to delete. In success, *semaphoreID = NULL.
 *
 * \return PAL_SUCCESS when the semaphore was deleted successfully, one of the following error codes in case of failure:
 * 		  PAL_ERR_RTOS_RESOURCE - Semaphore already released.
 * 		  PAL_ERR_RTOS_PARAMETER - Semaphore ID is invalid.
 * \note After this call, the semaphore_id is no longer valid and cannot be used.
 */
palStatus_t pal_plat_osSemaphoreDelete(palSemaphoreID_t* semaphoreID)
{
    palStatus_t status = PAL_SUCCESS;
    {
        if (NULL == semaphoreID)
        {
            return PAL_ERR_INVALID_ARGUMENT;
        }

        sem_t* sem = (sem_t*) (*semaphoreID);
        if (NULL == sem)
        {
            status = PAL_ERR_RTOS_RESOURCE;
            goto finish;
        }
        if (-1 == sem_destroy(sem))
        {
            status = PAL_ERR_RTOS_PARAMETER;
            goto finish;
        }

        if (NULL != sem)
        {
            free(sem);
        }
        *semaphoreID = (palSemaphoreID_t) NULL;
    }
    finish: return status;
}

/*! Perform an atomic increment for a signed32 bit value.
 *
 * @param[in,out] valuePtr The address of the value to increment.
 * @param[in] increment The number by which to increment.
 *
 * \returns The value of the valuePtr after the increment operation.
 */
int32_t pal_plat_osAtomicIncrement(int32_t* valuePtr, int32_t increment)
{
    int32_t res = __sync_add_and_fetch(valuePtr, increment);
    return res;
}


void *pal_plat_malloc(size_t len)
{
    return malloc(len);
}


void pal_plat_free(void * buffer)
{
    return free(buffer);
}

palStatus_t pal_plat_osRandomBuffer(uint8_t *randomBuf, size_t bufSizeBytes, size_t* actualRandomSizeBytes)
{
    palStatus_t status = PAL_SUCCESS;
	status = pal_plat_getRandomBufferFromHW(randomBuf, bufSizeBytes, actualRandomSizeBytes);
    return status;
}

#if (PAL_USE_HW_RTC)
#include <linux/rtc.h>
#include <sys/ioctl.h>
#include <time.h>
palMutexID_t rtcMutex = NULLPTR;

#if RTC_PRIVILEGE
static const char default_rtc[] = "/dev/rtc0";

PAL_PRIVATE  uint64_t pal_convertTimeStructToSeconds(const struct rtc_time *dateTime)
{
    /* Number of days from begin of the non Leap-year*/
    uint64_t monthDays[] = {0, 31U, 59U, 90U, 120U, 151U, 181U, 212U, 243U, 273U, 304U, 334U};
    uint64_t seconds, daysCount = 0;
    /* Compute number of days from 1970 till given year*/
    daysCount = (dateTime->tm_year + 1900 - 1970) * PAL_DAYS_IN_A_YEAR;
    /* Add leap year days */
    daysCount += (((dateTime->tm_year + 1900) / 4) - (1970U / 4));
    /* Add number of days till given month*/
    daysCount += monthDays[dateTime->tm_mon];
    /* Add days in given month minus one */
    daysCount += (dateTime->tm_mday - 1);
    if (!(((dateTime->tm_year + 1900) & 3U)) && (dateTime->tm_mon <= 2U))
    {
    	daysCount--;
    }

    seconds = (daysCount * PAL_SECONDS_PER_DAY) + (dateTime->tm_hour * PAL_SECONDS_PER_HOUR) +
              (dateTime->tm_min * PAL_SECONDS_PER_MIN) + dateTime->tm_sec;

    return seconds;
}
#endif

palStatus_t pal_plat_osGetRtcTime(uint64_t *rtcGetTime)
{
	palStatus_t ret = PAL_SUCCESS;
#if RTC_PRIVILEGE
    struct rtc_time GetTime ={0};
    if(rtcGetTime != NULL)
    {
        int fd, retval = 0;
        fd = open(default_rtc, O_RDONLY);
        if (fd == -1)
        {
            ret = PAL_ERR_RTOS_RTC_OPEN_DEVICE_ERROR;
        }
        else
        {
            retval = ioctl(fd, RTC_RD_TIME , &GetTime);
            if (retval == -1)
            {
                ret = PAL_ERR_RTOS_RTC_OPEN_IOCTL_ERROR;
            }
            else
            {
                *rtcGetTime = pal_convertTimeStructToSeconds(&GetTime);
            }
            close(fd);
        }
    }
    else
    {
        ret = PAL_ERR_NULL_POINTER;
    }
#else
    *rtcGetTime = time(NULL);
#endif
    return ret;
}

palStatus_t pal_plat_osSetRtcTime(uint64_t rtcSetTime)
{
	palStatus_t ret = 0;
	int retval = 0;
#if RTC_PRIVILEGE
    int fd = 0;
    int retval = 0;
    struct tm * convertedTime = gmtime((time_t*)&rtcSetTime);

    fd = open (default_rtc, O_RDONLY);
    retval = ioctl(fd, RTC_SET_TIME, (struct rtc_time*)convertedTime);
    if (retval == -1)
    {
        ret = PAL_ERR_RTOS_RTC_OPEN_IOCTL_ERROR;
    }
    close(fd);
#else
    ret = pal_osMutexWait(rtcMutex, 5 * PAL_MILLI_PER_SECOND * PAL_ONE_SEC);
    if(ret == PAL_SUCCESS)
    {
        retval = stime((time_t*)&rtcSetTime);
        if (retval == -1)
        {
            ret = PAL_ERR_RTOS_NO_PRIVILEGED; //need to give privilege mode "sudo setcap -v cap_sys_time=+epi [filename]"
        }
        pal_osMutexRelease(rtcMutex);
    }
#endif
    return ret;
}

palStatus_t pal_plat_rtcInit(void)
{
    palStatus_t ret = PAL_SUCCESS;
    if(NULLPTR == rtcMutex)
    {
        ret = pal_osMutexCreate(&rtcMutex);
    }
    return ret;
}

palStatus_t pal_plat_rtcDeInit(void)
{
    palStatus_t ret = PAL_SUCCESS;
    if(NULLPTR != rtcMutex)
    {
        ret = pal_osMutexDelete(&rtcMutex);
        rtcMutex = NULLPTR;
    }
    return ret;
}

#endif //#if (PAL_USE_HW_RTC)
