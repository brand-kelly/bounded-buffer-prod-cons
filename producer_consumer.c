#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <asm/param.h>
#include <linux/proc_fs.h>
#include <linux/time.h>
#include <linux/ktime.h>
#include <linux/time_namespace.h>
#include <linux/timer.h>
#include <linux/uidgid.h>

#define PCINFO(s, ...) pr_info("###[%s]###" s, __FUNCTION__, ##__VA_ARGS__)
#define MAX_BUFFER_SIZE 100


/* Important structs for kernel module. */
// Struct to hold the task information we are interested in
struct process_info {
    pid_t pid;
    unsigned long start_time;
	unsigned long boot_time;
};

// Qeueue to represent the buffer for storing process_info objects
struct queue {
    struct process_info *head;
    struct process_info *tail;
};

/* Global Variables */
unsigned long total_time_elapsed = 0;
int total_no_of_process_produced = 0;
int total_no_of_process_consumed = 0;
int use = 0, fill = 0, end_flag = 0, producer_complete = 0;
struct process_info buffer[MAX_BUFFER_SIZE];

static struct semaphore empty;
static struct semaphore full;
static struct semaphore mutex;
static struct task_struct **producer_thread;
static struct task_struct **consumer_thread;

/* Kernel Module parameters */
static int buffSize = 0;        // the initial value of the semaphore empty and bufferSize


static int prod = 0;            // number of producers
static int cons = 0;            // number of consumers (a non-negative number)
static int uuid = 0;            // UID of the user

module_param(buffSize, int, 0);
module_param(prod, int, 0);
module_param(cons, int, 0);
module_param(uuid, int, 0);

int producer_thread_function(void *pv) {
    struct task_struct *task;

    /* This produces an item by searching Linux's task_struct list
       and essentially "producing" a task for us to use when adding
       it to the buffer if the task's uid value matches the desired
       uid */
    for_each_process(task) {
        if (kthread_should_stop()) {
            break;
        }
        if (task->cred->uid.val == uuid) {
            if (down_interruptible(&empty)) {

                break;
            }

            if (down_interruptible(&mutex)) {

                up(&empty);
                break;
            }

            // Critical section: Add produced item to buffer
            buffer[fill].pid  = task->pid;
            buffer[fill].start_time = task->start_time;
            buffer[fill].boot_time = task->start_boottime;
            fill = (fill + 1) % buffSize;
            total_no_of_process_produced++;
            PCINFO("[%s] Produce-Item#:%d at buffer index: %d for PID:%d \n", current->comm,
                    total_no_of_process_produced, (fill + buffSize - 1) % buffSize, task->pid);

            up(&mutex); // Release the mutex semaphore
            up(&full);  // Increment the full semaphore
        }
    }
    PCINFO("[%s] Producer Thread Stopped.\n", current->comm);
    producer_complete = 1;
    return 0;
}

int consumer_thread_function(void *pv) {
    int no_of_process_consumed = 0;
    struct process_info consumer_item;
    unsigned long process_time_hr;
    unsigned long process_time_min;
    unsigned long process_time_sec;
    unsigned long start_time_ns;
    unsigned long ktime;
    unsigned long process_time_elapsed;

    while (!kthread_should_stop()) {
        
        if (down_interruptible(&full)) {
            continue;
        }
        if (down_interruptible(&mutex)) {
            up(&full);
            continue;
        }
        if (end_flag) {
            up(&mutex);
            up(&full);
            break;
        }
        // Critical section: Consume item

        consumer_item = buffer[use];
        use = (use + 1) % buffSize;
        no_of_process_consumed++;
        total_no_of_process_consumed++;

        up(&mutex); // Release the mutex semaphore
        up(&empty); // Increment the empty semaphore


        start_time_ns = consumer_item.start_time;
		ktime = ktime_get_ns();
		process_time_elapsed = (ktime - start_time_ns) / 1000000000;
		total_time_elapsed += ktime - start_time_ns;

		process_time_hr = process_time_elapsed / 3600;
		process_time_min = (process_time_elapsed - 3600 * process_time_hr) / 60;
		process_time_sec = (process_time_elapsed - 3600 * process_time_hr) - (process_time_min * 60);

        PCINFO("[%s] Consumed Item#-%d on buffer index:%d::PID:%d \t Elapsed Time %lu:%lu:%lu \n", current->comm,
		   no_of_process_consumed, (use + buffSize - 1) % buffSize, consumer_item.pid, process_time_hr, process_time_min, process_time_sec);

        if (end_flag) {
            break;
        }

    }
    PCINFO("[%s] Consumer Thread stopped.\n", current->comm);
    return 0;
}

static int __init my_init(void) {
    
    PCINFO("CSE 330 Project-4 Kernel Module Inserted\n");
    PCINFO("Kernel module received the following inputs: UID:%d, "
            "Buffer-Size:%d, No of Producer:%d, No of Consumer:%d\n",
            uuid, buffSize, prod, cons);
    // empty is initialized to n and full is initialized to 0
    sema_init(&empty, buffSize);
    sema_init(&full, 0);
    sema_init(&mutex, 1);

    if (buffSize > 0 && (prod >= 0 && prod < 2) && cons >= 0) {
        producer_thread = kmalloc(prod * sizeof(struct task_struct *), GFP_KERNEL);
        for (size_t i = 0; i < prod; ++i) {
            producer_thread[i] = kthread_run(producer_thread_function, NULL, "Producer-%ld", i + 1);
            if (IS_ERR(producer_thread[i])) {
                pr_err("Failed to create producer thread\n");
                return PTR_ERR(producer_thread[i]);
            } else {
                PCINFO("[Producer-%ld] kthread Producer Created Successfully\n", i + 1);
            }
        }

        consumer_thread = kmalloc(cons * sizeof(struct task_struct *), GFP_KERNEL);
        for (size_t i = 0; i < cons; ++i) {
            consumer_thread[i] = kthread_run(consumer_thread_function, NULL, "Consumer-%ld", i + 1);
            if (IS_ERR(consumer_thread[i])) {
                pr_err("Failed to create consumer thread\n");
                return PTR_ERR(consumer_thread[i]);
            } else {
                PCINFO("[Consumer-%ld] kthread Consumer Created Successfully\n", i + 1);
            }
        }
    } else {
        PCINFO("Incorrect Input Parameter Configuration Received. No kernel threads started. Please check input parameters.");
		PCINFO("The kernel module expects buffer size (a positive number) and # of producers(0 or 1) and # of consumers > 0");
    }
    return 0;
}

static void __exit my_exit(void) {
	if (buffSize > 0)
	{
        unsigned long total_time_hr;
        unsigned long total_time_min;
        unsigned long total_time_sec;

		while (1)
		{
			if (total_no_of_process_consumed == total_no_of_process_produced || !cons || !prod)
			{

				if (prod == 1 && producer_thread[0] && !producer_complete)
				{
					kthread_stop(producer_thread[0]);
				}
                
				end_flag = 1;

				for (int i = 0; i < cons; i++)
				{
					up(&full);
					up(&mutex);
				}
                
				for (int i = 0; i < cons; i++)
				{
					if (consumer_thread[i]){
						kthread_stop(consumer_thread[i]);
					}
				}
				break;
			}
			else
				continue;
		}

		// total_time_elapsed is now in sec
		total_time_elapsed = total_time_elapsed / 1000000000;

		total_time_hr = total_time_elapsed / 3600;
		total_time_min = (total_time_elapsed - 3600 * total_time_hr) / 60;
		total_time_sec = (total_time_elapsed - 3600 * total_time_hr) - (total_time_min * 60);

		PCINFO("Total number of items produced: %d", total_no_of_process_produced);
		PCINFO("Total number of items consumed: %d", total_no_of_process_consumed);
		PCINFO("The total elapsed time of all processes for UID %d is \t%lu:%lu:%lu  \n", uuid, total_time_hr, total_time_min, total_time_sec);
        kfree(producer_thread);
        kfree(consumer_thread);
	}

    PCINFO("CSE 330 Project-4 Kernel Module Removed\n");
}

module_init(my_init);
module_exit(my_exit);

MODULE_AUTHOR("Brandon Kelly");
MODULE_DESCRIPTION("Project 3 Producer Consumer");
MODULE_LICENSE("GPL");
