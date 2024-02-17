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

unsigned long long total_time_elapsed = 0;

// Struct to hold the task information we are interested in
struct process_info {
    pid_t pid;
    unsigned long long start_time;
	unsigned long long boot_time;
    struct process_info *next;
};

// Qeueue to represent the buffer for our problem
struct queue {
    struct process_info *head;
    struct process_info *tail;
};

int total_no_of_process_produced = 0;
int total_no_of_process_consumed = 0;
int use = 0, fill = 0, end_flag = 0;

static int buffSize = 0;        // the initial value of the semaphore empty and bufferSize
module_param(buffSize, int, 0);

struct queue* buffer;

static int prod = 0;            // number of producers
module_param(prod, int, 0);

static int cons = 0;            // number of consumers (a non-negative number)
module_param(cons, int, 0);

static int uuid = 0;            // UID of the user
module_param(uuid, int, 0);

static struct semaphore empty;
static struct semaphore full;
static struct semaphore mutex;

static struct task_struct **producer_thread;
static struct task_struct **consumer_thread;

struct queue *init_queue(void) {
    struct queue* buffer = kmalloc(sizeof(struct queue), GFP_KERNEL);
    if (buffer) { 
        buffer->head = NULL;
        buffer->tail = NULL;
    }
    return buffer;
}

void free_queue(struct queue* buffer) {
    struct process_info* curr = buffer->head;
    while (curr) {
        struct process_info* next = curr->next;
        kfree(curr);
        curr = next;
    }
    kfree(buffer);
}

void enqueue(struct queue* buffer, struct process_info* item) {
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
        if (kthread_should_stop()) {
            break;
        }
        if (task->cred->uid.val == uuid) {
            // Produce Item
            struct process_info *new_item = kmalloc(sizeof(struct process_info), GFP_KERNEL);
            new_item->pid = task->pid;
            new_item->start_time = task->start_time;
            new_item->boot_time = task->start_boottime;
            new_item->next = NULL;
            // Acquire semaphore locks
            if (down_interruptible(&empty)) {
                break;
            }

            if (down_interruptible(&mutex)) {
                up(&empty);
                break;
            }

            // Critical section: Add produced item to buffer
            if (fill < buffSize) {
                enqueue(buffer, new_item);
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
    PCINFO("We are in [%s]\n", current->comm}
    while (!kthread_should_stop()) {
        if (end_flag == 1) {
            break;
        }
        if (down_interruptible(&full)) {
            continue;
        }
        PCINFO("[%s] acquired full semaphore\n", current->comm}
        if (down_interruptible(&mutex)) {
            up(&full);
            continue;
        }
        PCINFO("[%s] acquired mutex semaphore\n", current->comm}
        if (end_flag == 1) {
            break;
        }

        // Critical section: Consume item
        if (use < buffSize) {
            struct process_info* item;
            item = dequeue(buffer);
            use++;

            unsigned long long start_time_ns = item->start_time;
		    unsigned long long ktime = ktime_get_ns();
		    unsigned long long process_time_elapsed = (ktime - start_time_ns) / 1000000000;
		    total_time_elapsed += ktime - start_time_ns;

		    unsigned long long process_time_hr = process_time_elapsed / 3600;
		    unsigned long long process_time_min = (process_time_elapsed - 3600 * process_time_hr) / 60;
		    unsigned long long process_time_sec = (process_time_elapsed - 3600 * process_time_hr) - (process_time_min * 60);

            no_of_process_consumed++;
            total_no_of_process_consumed++;
            PCINFO("[%s] Consumed Item#-%d on buffer index:%d::PID:%d \t Elapsed Time %llu:%llu:%llu \n", current->comm,
			   no_of_process_consumed, (use + buffSize - 1) % buffSize, item->pid, process_time_hr, process_time_min, process_time_sec);
        }

        up(&mutex); // Release the mutex semaphore
        up(&empty); // Increment the empty semaphore

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
        buffer = init_queue();

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
	if (buffSize > 0)
	{
		while (1)
		{
			if (total_no_of_process_consumed == total_no_of_process_produced || !cons || !prod)
			{
				if (!cons)
				{
					up(&empty);
				}

				for (int i = 0; i < prod; i++)
				{
					if (producer_thread[i])
					{
						kthread_stop(producer_thread[i]);
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
						kthread_stop(consumer_thread[i]);
					}
				}
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
	}

    pr_info("Module unloaded\n");
    kfree(producer_thread);
    kfree(consumer_thread);
    free_queue(buffer);
}

module_init(my_init);
module_exit(my_exit);

MODULE_AUTHOR("Brandon Kelly");
MODULE_DESCRIPTION("Project 3 Producer Consumer");
MODULE_LICENSE("GPL");