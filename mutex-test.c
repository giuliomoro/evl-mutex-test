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

typedef pthread_mutex_t mutex_t;
#endif

mutex_t mutex;
volatile int shouldStop;
pthread_t* threads;

void* threadFunc(void* arg)
{
	int n = (long int)arg;
	evl_printf("Thread %d\n", n);
	int ret = evl_attach_thread(EVL_CLONE_PRIVATE, "thread-%d", n);
	if(ret < 0)
	{
		fprintf(stderr, "evl_attach_thread for thread %d failed with %s (%d)\n", n, strerror(-ret), -ret);
		return NULL;
	}
	cpu_set_t readset;
	ret = pthread_getaffinity_np(threads[n], sizeof(readset), &readset);
	if(ret)
	{
		fprintf(stderr, "pthread_getaffinity_np %d %s\n", ret, strerror(ret));
		return NULL;
	}
	char str[1000];
	char* p = str;
	// assemble the string before evl_printf to avoid scrambling because of
	// writing from multiple threads
	p += snprintf(p, sizeof(str) - (p - str), "Affinity for %d: [ ", n);
	for (int j = 0; j < CPU_SETSIZE; j++)
		if (CPU_ISSET(j, &readset))
		{
			if(p < str + sizeof(str));
				p += snprintf(p, sizeof(str) - (p - str), "%d  ", j);
		}
	evl_printf("%s]\n", str);
	while(!shouldStop)
	{
		evl_printf("thread %d locking\n", n);
		int ret = evl_lock_mutex(&mutex);
		if(ret)
			evl_printf("thread %d ERROR lock() %d %s\n", n, ret, strerror(-ret));
		else
		{
			evl_printf("thread %d locked\n", n);
			for(volatile unsigned int n = 0; n < 100000; ++n) // waste time
				;
			ret = evl_unlock_mutex(&mutex);
			if(ret)
				evl_printf("thread %d ERROR unlock() %d %s\n", n, ret, strerror(-ret));
			else
				evl_printf("thread %d unlocked\n", n);
		}
	}
	return NULL;
}

int main(int argc, char** argv)
{
	int maxThreads = get_nprocs();
	if(argc < 3)
	{
		fprintf(stderr, "Usage \"test numThreads mainPrio\"\n");
		return 1;
	}
	int numThreads = atoi(argv[1]);
	if(numThreads > maxThreads)
	{
		fprintf(stderr, "Too many threads\n");
		return 1;
	}
	int mainPrio = atoi(argv[2]);
	if(mainPrio > sched_get_priority_max(SCHED_FIFO))
	{
		fprintf(stderr, "Too high priority\n");
		return 1;
	}
	printf("NumThreads: %d\n", numThreads);
	threads = (pthread_t*)malloc(sizeof(threads[0]) * numThreads);
	int threadPrio = 94;
	int ret = evl_init();
	if(ret < 0)
	{
		fprintf(stderr, "evl_init: %d %s\n", ret, strerror(-ret));
		return 1;
	}
	ret = evl_create_mutex(&mutex, EVL_CLOCK_MONOTONIC, 0, EVL_MUTEX_NORMAL | EVL_CLONE_PRIVATE, "test-mutex");
	if(ret < 0)
	{
		fprintf(stderr, "evl_create_mutex: %d %s\n", ret, strerror(-ret));
		return 1;
	}
	ret = evl_attach_self("main");
	if(ret < 0)
	{
		fprintf(stderr, "evl_attach_self: %d %s\n", ret, strerror(-ret));
		return 1;
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
		return 1;
	}
	int policy = mainPrio ? SCHED_FIFO : SCHED_OTHER;
	struct sched_param param;
	param.sched_priority = mainPrio;
	ret = pthread_setschedparam(pthread_self(), policy, &param);
	if(ret)
	{
		fprintf(stderr, "pthread_setschedparam: %d %s\n", ret, strerror(ret));
		return 1;
	}
	ret = evl_lock_mutex(&mutex);
	if(ret < 0)
	{
		fprintf(stderr, "evl_lock_mutex: %d %s\n", ret, strerror(-ret));
		return 1;
	}
	for(unsigned int n = 0; n < numThreads; ++n)
	{
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(n, &cpuset);
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
			return 1;
		}
	}
	evl_printf("All threads go\n");
	ret = evl_unlock_mutex(&mutex);
	if(ret)
	{
		evl_printf("main ERROR unlock before loop\n");
		return 1; // TODO: cleanup
	}
	evl_printf("main unlocked before loop\n");

	int err = 0;
	for(unsigned int n = 0; n < 100000; ++n)
	{
		evl_printf("main try_lock %d\n", n);
		int ret = evl_trylock_mutex(&mutex);
		if(0 == ret)
		{
			evl_printf("main locked\n");
			ret = evl_unlock_mutex(&mutex);
			if(ret) {
				evl_printf("main ERROR unlock: %d %s\n", ret, strerror(-ret));
				shouldStop = 1;
				err++;
				break;
			}
			else
				evl_printf("main unlocked %d\n");
		} else if (-EBUSY == ret) {
			evl_printf("main try_lock: busy\n");
		} else {
			evl_printf("main ERROR try_lock: %d %s\n", ret, strerror(-ret));
			shouldStop = 1;
			err++;
			break;
		}
		evl_usleep(100);
	}
	shouldStop = 1;
	for(unsigned int n = 0; n < numThreads; ++n)
	{
		printf("Joining %d\n", n);
		pthread_join(threads[n], NULL);
		printf("Joined %d\n", n);
	}
	free(threads);
	return err;
}
