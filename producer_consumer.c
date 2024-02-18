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



/* Important structs for kernel module. */
// Struct to hold the task information we are interested in
struct process_info {
    pid_t pid;
    unsigned long long start_time;
	unsigned long long boot_time;
    struct process_info *next;
};

// Qeueue to represent the buffer for storing process_info objects
struct queue {
    struct process_info *head;
    struct process_info *tail;
};

/* Global Variables */
unsigned long long total_time_elapsed = 0;
int total_no_of_process_produced = 0;
int total_no_of_process_consumed = 0;
int use = 0, fill = 0, end_flag = 0;

struct queue* buffer;

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


    /*
        Dynamically allocates memory for the buffer as a queue for
        the Bounded-Buffer Problem. The buffer is initialized as a
        global variable above and function is invoked in main.
        :rtype:
            struct queue
    */
void init_queue(void) {
    buffer = kmalloc(sizeof(struct queue), GFP_KERNEL);
    if (buffer) { 
        buffer->head = NULL;
        buffer->tail = NULL;
    }
}

void free_queue(struct queue *buffer) {
    if (buffer) {
        struct process_info *curr = buffer->head;
        while (curr) {
            struct process_info *next = curr->next;
            kfree(curr);    // Free objects stored in buffer
            curr = next;
        }
        kfree(buffer); // free the buffer
        PCINFO("Buffer memory freed");
    }
}

void enqueue(struct process_info* item) {
    if (buffer->head == NULL) {
        buffer->head = item;
        buffer->tail = item;
    } else {
        buffer->tail->next = item;
        buffer->tail = item;
    }
}

struct process_info* dequeue(struct queue* buffer) {
    if (buffer->head == NULL) {
        return NULL;
    } else {
        struct process_info* item = buffer->head;
        buffer->head = buffer->head->next;
        if (buffer->head == NULL) {
            buffer->tail = NULL;
        }
        return item;
    }
}

int producer_thread_function(void *pv) {
    struct task_struct *task;

    /* This produces an item by searching Linux's task_struct list
       and essentially "producing" a task for us to use when adding
       it to the buffer if the task's uid value matches the desired
       uid */
    for_each_process(task) {
        if (task->cred->uid.val == uuid) {
            // Produce Item
            struct process_info *new_item = kmalloc(sizeof(struct process_info), GFP_KERNEL);
            if (new_item) {
                new_item->pid = task->pid;
                new_item->start_time = task->start_time;
                new_item->boot_time = task->start_boottime;
                new_item->next = NULL;
            } else {
                pr_err("Failed to allocate memory for produced item\n");
                return 1;
            }
            // Acquire semaphore locks
            if (down_interruptible(&empty)) {
                break;
            }

            if (down_interruptible(&mutex)) {
                break;
            }

            // Critical section: Add produced item to buffer
            if (fill < buffSize) {
                enqueue(new_item);
                fill++;
                total_no_of_process_produced++;

                PCINFO("[%s] Produce-Item#:%d at buffer index: %d for PID:%d \n", current->comm,
                        total_no_of_process_produced, (fill + buffSize - 1) % buffSize, task->pid);
            } else {
                kfree(new_item);
            }
            // Release locks
            up(&mutex); // Release the mutex semaphore
            up(&full);  // Increment the full semaphore
        }
    }
    PCINFO("[%s] Producer Thread Stopped.\n", current->comm);
    return 0;
}

int consumer_thread_function(void *pv) {
    int no_of_process_consumed = 0;
    struct process_info *consumer_item;
    while (!kthread_should_stop()) {
        
        if (down_interruptible(&full)) {
            break;
        }
        if (down_interruptible(&mutex)) {
            break;
        }

        PCINFO("We are in [%s]'s Critical Section\n", current->comm);
        // Critical section: Consume item
        if (use < buffSize) {

            consumer_item = dequeue(buffer);
            use++;
            no_of_process_consumed++;
            total_no_of_process_consumed++;
        }

        up(&mutex); // Release the mutex semaphore
        up(&empty); // Increment the empty semaphore

        if (consumer_item) {
            unsigned long long start_time_ns = consumer_item->start_time;
		    unsigned long long ktime = ktime_get_ns();
		    unsigned long long process_time_elapsed = (ktime - start_time_ns) / 1000000000;
		    total_time_elapsed += ktime - start_time_ns;

		    unsigned long long process_time_hr = process_time_elapsed / 3600;
		    unsigned long long process_time_min = (process_time_elapsed - 3600 * process_time_hr) / 60;
		    unsigned long long process_time_sec = (process_time_elapsed - 3600 * process_time_hr) - (process_time_min * 60);

            PCINFO("[%s] Consumed Item#-%d on buffer index:%d::PID:%d \t Elapsed Time %llu:%llu:%llu \n", current->comm,
			   no_of_process_consumed, (use + buffSize - 1) % buffSize, consumer_item->pid, process_time_hr, process_time_min, process_time_sec);

            consumer_item = NULL;
        }
    }
    PCINFO("[%s] Consumer Thread stopped.\n", current->comm);
    return 0;
}

static int __init my_init(void) {
    
    pr_info("Module loaded\n");
    // empty is initialized to n and full is initialized to 0
    sema_init(&empty, buffSize);
    sema_init(&full, 0);
    sema_init(&mutex, 1);

    if (buffSize > 0 && (prod >= 0 && prod < 2) && cons >= 0) {
        init_queue();
        if (!buffer) {
            pr_err("Failed to allocate memory to buffer");
            return PTR_ERR(buffer);
        } 
        producer_thread = kmalloc(prod * sizeof(struct task_struct *), GFP_KERNEL);
        for (int i = 0; i < prod; i++) {
            producer_thread[i] = kthread_run(producer_thread_function, NULL, "Producer-%d", i);
            if (IS_ERR(producer_thread[i])) {
                pr_err("Failed to create producer thread\n");
                return PTR_ERR(producer_thread[i]);
            }
        }

        consumer_thread = kmalloc(cons * sizeof(struct task_struct *), GFP_KERNEL);
        for (int i = 0; i < cons; i++) {
            consumer_thread[i] = kthread_run(consumer_thread_function, NULL, "Consumer-%d", i);
            if (IS_ERR(consumer_thread[i])) {
                pr_err("Failed to create consumer thread\n");
                return PTR_ERR(consumer_thread[i]);
            }
        }
    } else {
        PCINFO("Incorrect Input Parameter Configuration Received. No kernel threads started. Please check input parameters.");
		PCINFO("The kernel module expects buffer size (a positive number) and # of producers(0 or 1) and # of consumers > 0");
    }
    return 0;
}

static void __exit my_exit(void) {
    PCINFO("Entering exit function\n");
	if (buffSize > 0)
	{
        PCINFO("buffSize > 0 succeeds\n");

		while (1)
		{
            PCINFO("total_no_of_process_consumed:%d == total_no_of_process_produced:%d or !cons:%d or !prod:%d\n", total_no_of_process_consumed, total_no_of_process_produced, cons, prod);
			if (total_no_of_process_consumed == total_no_of_process_produced || !cons || !prod)
			{
				if (!cons)
				{
                    PCINFO("Inside if not cons");
					up(&empty);
				}
                int prod_ret = -1;
                int cons_ret = -1;
				for (int i = 0; i < prod; i++)
				{
					if (producer_thread[i])
					{
                        PCINFO("Before stopping producer thread");
						prod_ret = kthread_stop(producer_thread[i]);
                        PCINFO("Producer-%d: stopped with return value %d", i, prod_ret);
					}
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
						cons_ret = kthread_stop(consumer_thread[i]);
                        PCINFO("Consumer-%d: stopped with return value %d", i, cons_ret);
					}
				}
                PCINFO("At the end of exit if statement in while loop line 287\n");
				break;
			}
			else
				continue;
		}

		// total_time_elapsed is now in nsec
		total_time_elapsed = total_time_elapsed / 1000000000;

		unsigned long long total_time_hr = total_time_elapsed / 3600;
		unsigned long long total_time_min = (total_time_elapsed - 3600 * total_time_hr) / 60;
		unsigned long long total_time_sec = (total_time_elapsed - 3600 * total_time_hr) - (total_time_min * 60);

		PCINFO("Total number of items produced: %d", total_no_of_process_produced);
		PCINFO("Total number of items consumed: %d", total_no_of_process_consumed);
		PCINFO("The total elapsed time of all processes for UID %d is \t%llu:%llu:%llu  \n", uuid, total_time_hr, total_time_min, total_time_sec);
        kfree(producer_thread);
        kfree(consumer_thread);
        free_queue(buffer);
	}

    pr_info("Module unloaded\n");
}

module_init(my_init);
module_exit(my_exit);

MODULE_AUTHOR("Brandon Kelly");
MODULE_DESCRIPTION("Project 3 Producer Consumer");
MODULE_LICENSE("GPL");