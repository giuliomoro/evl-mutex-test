#define _GNU_SOURCE
#include <sched.h>
#include <unistd.h>
#ifndef USE_PTHREAD
#include <evl/evl.h>
#endif // USE_PTHREAD
#include <pthread.h>
#include <string.h>
#include <error.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>

#define ERR_OTHER (1 << 0)
#define ERR_TRYLOCK (1 << 1)
#define ERR_UNLOCK (1 << 2)
#define ERR_LOCK (1 << 3)
#define ERR_BITS 4

#define PRINT_NONE 1
#define PRINT_ERRORS 2
#define PRINT_THROTTLED 3
#define PRINT_ALL 4
int printFlags = PRINT_ALL;
const int throttledPrints = 500;
int useMutex = 1;

#ifdef __EVL__
typedef struct evl_mutex mutex_t;
#else
typedef pthread_mutex_t mutex_t;
int nop(){ return 0; }
#define evl_init(...) nop()
#define evl_attach_thread(...) nop()
#define evl_attach_self(...) nop()
#define evl_printf(...) printf(__VA_ARGS__)
#define evl_create_mutex(mutex, ...) \
	-pthread_mutex_init(mutex, NULL);

int evl_lock_mutex(mutex_t* mutex)
{
	return -pthread_mutex_lock(mutex);
}
int evl_trylock_mutex(mutex_t* mutex)
{
	return -pthread_mutex_trylock(mutex);
}
int evl_unlock_mutex(mutex_t* mutex)
{
	return -pthread_mutex_unlock(mutex);
}
#define evl_usleep(...) usleep(__VA_ARGS__)
#endif

mutex_t mutex;
volatile int shouldStop;
pthread_t* threads;

void* kThreadSuccess = (void*)2l;
void* kThreadError = (void*)1l;

void* threadFunc(void* arg)
{
	int n = (long int)arg;
	int shouldPrint = (PRINT_NONE != printFlags);
	shouldPrint && evl_printf("Thread %d\n", n);
	int ret = evl_attach_thread(EVL_CLONE_PRIVATE, "thread-%d", n);
	if(ret < 0)
	{
		fprintf(stderr, "evl_attach_thread for thread %d failed with %s (%d)\n", n, strerror(-ret), -ret);
		return (void*)ERR_OTHER;
	}
	cpu_set_t readset;
	ret = pthread_getaffinity_np(threads[n], sizeof(readset), &readset);
	if(ret)
	{
		fprintf(stderr, "pthread_getaffinity_np %d %s\n", ret, strerror(ret));
		return (void*)ERR_OTHER;
	}
	char str[1000];
	char* p = str;
	// assemble the string before evl_printf to avoid scrambling because of
	// writing from multiple threads
	if(shouldPrint)
	{
		p += snprintf(p, sizeof(str) - (p - str), "Affinity for %d: [ ", n);
		for (int j = 0; j < CPU_SETSIZE; j++)
			if (CPU_ISSET(j, &readset))
			{
				if(p < str + sizeof(str));
					p += snprintf(p, sizeof(str) - (p - str), "%d  ", j);
			}
		evl_printf("%s]\n", str);
	}
	long int err = 0;
	unsigned int c = 0;
#ifdef __EVL__
	evl_switch_oob();
#endif // __EVL__
	while(!shouldStop)
	{
		int shouldPrint = (PRINT_ALL == printFlags) || ((PRINT_THROTTLED == printFlags) && (0 == (n % throttledPrints)));
		int shouldPrintError = (PRINT_NONE != printFlags);
		c++;
		shouldPrint && evl_printf("thread %d waiting\n", n);
		int ret = 0;
		useMutex && (ret = evl_lock_mutex(&mutex));
		if(ret) {
			shouldPrintError && evl_printf("thread %d ERROR lock() %d %s\n", n, ret, strerror(-ret));
			err |= ERR_LOCK;
		} else {
			shouldPrint && evl_printf("thread %d locked\n", n);
			for(volatile unsigned int n = 0; n < 100000; ++n) // do some work
				;
			useMutex && (ret = evl_unlock_mutex(&mutex));
			if(ret) {
				shouldPrintError && evl_printf("thread %d ERROR unlock() %d %s\n", n, ret, strerror(-ret));
				err |= ERR_UNLOCK;
			} else {
				shouldPrint && evl_printf("thread %d unlocked\n", n);
			}
		}
	}
	return (void*)err;
}

void sig_handler(int var)
{
	shouldStop = 1;
}


int main(int argc, char** argv)
{
#ifdef __EVL__
	char backend[] = "EVL";
#else
	char backend[] = "pthread";
#endif
	int maxThreads = get_nprocs();
	if(argc < 4)
	{
		fprintf(stderr, "Usage \"%s numIter numThreads mainPrio [flags]\"\n"
				"Where flags is a combination of:\n"
				" k: keep going on error (default: no)\n"
				" q: print nothing\n"
				" e: print errors\n"
				" t: print every %d iterations or errors\n"
				" a: print all (default)\n"
				" m: do not use mutex\n"
				, argv[0], throttledPrints);
		return ERR_OTHER;
	}
	int numIter = atoi(argv[1]);
	int numThreads = atoi(argv[2]);
	if(numThreads > maxThreads)
	{
		fprintf(stderr, "Too many threads\n");
		return ERR_OTHER;
	}
	int mainPrio = atoi(argv[3]);
	if(mainPrio > sched_get_priority_max(SCHED_FIFO))
	{
		fprintf(stderr, "Too high priority\n");
		return ERR_OTHER;
	}
	int keepGoing = 0;
	if(argc >= 5)
	{
		const char* str = argv[4];
		for(unsigned int n = 0; n < strlen(str); ++n)
		{
			switch (str[n])
			{
			case 'k':
				keepGoing = 1;
				break;
			case 'e':
				printFlags = PRINT_ERRORS;
				break;
			case 'q':
				printFlags = PRINT_NONE;
				break;
			case 't':
				printFlags = PRINT_THROTTLED;
				break;
			case 'a':
				printFlags = PRINT_ALL;
				break;
			case 'm':
				useMutex = 0;
				break;
			}
		}
	}
	int shouldPrint = (PRINT_NONE != printFlags);
	shouldPrint && printf("Using %s\n", backend);
	shouldPrint && printf("NumThreads: %d\n", numThreads);

	int threadPrio = 94;
	signal(SIGINT, sig_handler);
	threads = (pthread_t*)malloc(sizeof(threads[0]) * numThreads);
	int ret = evl_init();
	if(ret < 0)
	{
		fprintf(stderr, "evl_init: %d %s\n", ret, strerror(-ret));
		return ERR_OTHER;
	}
	ret = evl_create_mutex(&mutex, EVL_CLOCK_MONOTONIC, 0, EVL_MUTEX_NORMAL | EVL_CLONE_PRIVATE, "test-mutex");
	if(ret < 0)
	{
		fprintf(stderr, "evl_create_mutex: %d %s\n", ret, strerror(-ret));
		return ERR_OTHER;
	}
	// set scheduling policy before attaching to the EVL core so that they
	// get properly inherited
	int policy = mainPrio ? SCHED_FIFO : SCHED_OTHER;
	struct sched_param param;
	param.sched_priority = mainPrio;
	ret = pthread_setschedparam(pthread_self(), policy, &param);
	if(ret)
	{
		fprintf(stderr, "pthread_setschedparam: %d %s\n", ret, strerror(ret));
		return ERR_OTHER;
	}
	ret = evl_attach_self("main");
	if(ret < 0)
	{
		fprintf(stderr, "evl_attach_self: %d %s\n", ret, strerror(-ret));
		return ERR_OTHER;
	}
	// EVL will have pinned this thread on the current CPU. Allow it to run
	// on all CPUs. Critically, this needs to be done before alowing
	// threads to run, or we may be starved by whichever one runs on our core.
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	for(unsigned int n = 0; n < maxThreads; ++n)
		CPU_SET(n, &cpuset);
	ret = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
	if(ret)
	{
		fprintf(stderr, "pthread_setaffinity_np: %d %s\n", ret, strerror(ret));
		return ERR_OTHER;
	}
	ret = evl_lock_mutex(&mutex);
	if(ret < 0)
	{
		fprintf(stderr, "evl_lock_mutex: %d %s\n", ret, strerror(-ret));
		return ERR_OTHER;
	}
	for(unsigned int n = 0; n < numThreads; ++n)
	{
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		// start from the last CPU to avoid using 100% of CPU 0 when
		// there is no other thread contending for the lock
		CPU_SET(maxThreads - 1 - n, &cpuset);
		pthread_attr_t attr;
		ret = pthread_attr_init(&attr);
		ret |= pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
		ret |= pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
		ret |= pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
		ret |= pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
		struct sched_param param;
		param.sched_priority = threadPrio;
		ret |= pthread_attr_setschedparam(&attr, &param);
		ret |= pthread_create(&threads[n], &attr, threadFunc, (void*)(long int)n);
		if(ret)
		{
			fprintf(stderr, "An error occurred while creating thread %d\n", n);
			return ERR_OTHER;
		}
	}
	shouldPrint && evl_printf("All threads go\n");
	ret = evl_unlock_mutex(&mutex);
	if(ret)
	{
		evl_printf("main ERROR unlock before loop\n");
		return ERR_OTHER; // TODO: cleanup
	}
	shouldPrint && evl_printf("main unlocked before loop\n");

	long int err = 0;
#ifdef __EVL__
	// necessary for when useMutex is false in order to force switching to OOB
	evl_switch_oob();
#endif // __EVL__
	for(unsigned int n = 0; n < numIter && !shouldStop; ++n)
	{
		int shouldPrint = (PRINT_ALL == printFlags) || ((PRINT_THROTTLED == printFlags) && (0 == (n % throttledPrints)));
		int shouldPrintError = (PRINT_NONE != printFlags);
		shouldPrint && evl_printf("main try_lock %d\n", n);
		int ret = 0;
		useMutex && (ret = evl_trylock_mutex(&mutex));
		if(0 == ret)
		{
			shouldPrint && evl_printf("main locked\n");
			useMutex && (ret = evl_unlock_mutex(&mutex));
			if(ret) {
				shouldPrintError && evl_printf("main ERROR unlock: %d %s\n", ret, strerror(-ret));
				err |= ERR_UNLOCK;
				if(!keepGoing)
					break;
			}
			else
				shouldPrint && evl_printf("main unlocked\n");
		} else if (-EBUSY == ret) {
			shouldPrint && evl_printf("main try_lock: busy\n");
		} else {
			shouldPrintError && evl_printf("main ERROR try_lock: %d %s\n", ret, strerror(-ret));
			err |= ERR_TRYLOCK;
			if(!keepGoing)
				break;
		}
		evl_usleep(100);
	}
	shouldStop = 1;
	for(unsigned int n = 0; n < numThreads; ++n)
	{
		shouldPrint && evl_printf("Joining %d\n", n);
		void* res;
		ret = pthread_join(threads[n], &res);
		shouldPrint && evl_printf("Joined %d : %s; result is %p\n", n, ret ? strerror(ret) : "", res);
		err |= (((long int)res) << ERR_BITS);
	}
	free(threads);
	if(err)
		shouldPrint && evl_printf("ERROR\n");
	else
		shouldPrint && evl_printf("SUCCESS\n");
	return err;
}
