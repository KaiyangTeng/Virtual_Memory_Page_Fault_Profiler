#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include "mp3_given.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kaiyang Teng <Kteng4@illinois.edu>");
MODULE_DESCRIPTION("CS-423 MP3");

// #define DEBUG 1
#define buff_size (128 * PAGE_SIZE)
static struct proc_dir_entry *mp3dir;
static struct proc_dir_entry *mp3status;
static struct list_head mp3head;
static struct mutex mp3mutex;
static struct workqueue_struct *wq;
static struct delayed_work worker;
static unsigned long *gbuffer;
static dev_t mp3dev;
static struct cdev mp3cdev;
int indx=0;

struct listnode
{
   pid_t pid;
   struct list_head node;
};


/*
 * Periodic profiler callback.
 *
 * Every time the delayed work runs, it samples all registered tasks and
 * accumulates four values for this sampling interval:
 *   1 current jiffies
 *   2 total soft faults
 *   3 total hard faults
 *   4 total CPU time (utime + stime)
 * Dead or invalid PIDs are removed from the registration list so that the
 * profiler does not keep polling stale tasks forever.
 */
static void workhandler(struct work_struct *input)
{
   unsigned long timesum=jiffies,minfault=0,majfault=0,cpu=0;
   struct listnode *curr,*temp;
   bool empty=true;
   // Sum the current sample over all registered tasks
   mutex_lock(&mp3mutex);
   list_for_each_entry_safe(curr,temp,&mp3head,node)
   {
      unsigned long minf,majf,ut,st;
      if(get_cpu_use(curr->pid,&minf,&majf,&ut,&st)==0)
      {
         empty=false;
         minfault+=minf;
         majfault+=majf;
         cpu+=ut+st;
      }
      else
      {
         list_del(&curr->node);
         kfree(curr);
      }
   }
   mutex_unlock(&mp3mutex);
   /*
    * Write one sample into the profiler buffer.
    * The monitor expects samples in groups of four unsigned long values.
    */
   if(!empty) 
   {
      gbuffer[indx++]=timesum;
      indx%=48000;
      gbuffer[indx++]=minfault;
      indx%=48000;
      gbuffer[indx++]=majfault;
      indx%=48000;
      gbuffer[indx++]=cpu;
      indx%=48000;
      // Re-arm the delayed work to keep sampling at 20Hz.
      mod_delayed_work(wq,&worker,msecs_to_jiffies(50));
   }
}

/*
 * Register one PID into the profiler list.
 *
 * If this PID is the first registered task, start a fresh profiling session
 * by resetting the shared buffer and starting the delayed work queue.
 */
static bool taskregister(pid_t pid)
{
   bool empty;
   struct listnode *temp;
   struct listnode *curr;
   if(pid<=0) return 0;
   curr=kmalloc(sizeof(*curr),GFP_KERNEL);
   if(!curr) 
   {
      pr_info("reg kmalloc failed\n");
      return 0;
   }
   curr->pid=pid;
   mutex_lock(&mp3mutex);
   empty=list_empty(&mp3head);
   list_for_each_entry(temp,&mp3head,node)
   {
      // Ignore duplicate registration 
      if(temp->pid==pid)
      {
         mutex_unlock(&mp3mutex);
         kfree(curr);
         pr_info("task already exist pid=%d \n",pid);
         return 1;
      }
   }
   list_add_tail(&curr->node,&mp3head);
   mutex_unlock(&mp3mutex);
   //Start sampling when the first task is inserted
   if(empty) queue_delayed_work(wq,&worker,0);
   return 1;
}

/*
 * Remove one PID from the profiler list.
 * If the list becomes empty after deletion, stop the delayed work because
 * there is no remaining task to profile.
 */
static bool deregister(pid_t pid)
{
   struct listnode *curr,*temp;
   bool empty;
   mutex_lock(&mp3mutex);
   list_for_each_entry_safe(curr,temp,&mp3head,node)
   {
      if(curr->pid==pid)
      {
         list_del(&curr->node);
         kfree(curr);
         empty=list_empty(&mp3head);
         mutex_unlock(&mp3mutex);
         //Stop sampling when there is no registered task left
         if(empty) cancel_delayed_work_sync(&worker);
         return 1;
      }
   }
   mutex_unlock(&mp3mutex);
   return 0;
}

/* Free every node left in the registration list during module cleanup. */
static void cleanlist(void)
{
   struct listnode *curr,*temp;
   mutex_lock(&mp3mutex);
   list_for_each_entry_safe(curr,temp,&mp3head,node)
   {
      list_del(&curr->node);
      kfree(curr);
   }
   mutex_unlock(&mp3mutex);
}

/*
 * Read callback for /proc/mp3/status.
 * Exports the current registered PID list to user space, one PID per line.
 */
static ssize_t my_read(struct file * inputfile, char __user * usrbuf, size_t len, loff_t * p)
{
   char *kbuf;
   struct listnode *cur;
   int total=0,left=0,cap=2048;
   kbuf=kmalloc(cap,GFP_KERNEL);
   if(!kbuf)
   {
      pr_info("kmalloc failed\n");
      return -1;
   }
   //Export the current registered PID list through /proc/mp3/status
   mutex_lock(&mp3mutex);
   list_for_each_entry(cur,&mp3head,node)
   {
      if(total>=cap-8) break;
      total+=scnprintf(kbuf+total,cap-total,"%d\n",cur->pid);
   }
   mutex_unlock(&mp3mutex);
   if(*p>=total)
   {
      kfree(kbuf);
      return 0;
   }
   left=total-*p;
   if(left<len) len=left;
   if(copy_to_user(usrbuf,kbuf+*p,len)) 
   {
      kfree(kbuf);
      pr_info("copy_to_user failed\n");
      return -1;
   }
   *p+=len;
   kfree(kbuf);
   return len;
}

/*
 * Write callback for /proc/mp3/status.
 *
 * Supported commands:
 *   R <pid> : register one PID
 *   U <pid> : unregister one PID
 */
static ssize_t my_write(struct file * inputfile, const char __user *usrbuf, size_t len, loff_t * p)
{
   char kbuf[25];
   if(len>=25||len==0) 
   {
      pr_info("Invalid input\n");
      return -1;
   }
   if(copy_from_user(kbuf,usrbuf,len))
   {
      pr_info("copy_from_user failed\n");
      return -1;
   }
   kbuf[len]='\0';
   //Parse "R <pid>" or "U <pid>" commands from user space
   if(kbuf[0]=='R')
   {
      int pid;
      if(sscanf(kbuf,"R %d",&pid)==1)
      {
         taskregister(pid);
      }
      else 
      {
         pr_info("Bad input R\n");
         return -1;
      }

   }
   else if(kbuf[0]=='U')
   {
      int pid;
      if(sscanf(kbuf,"U %d",&pid)==1)
      {
         deregister(pid);
      }
      else 
      {
         pr_info("Bad input U\n");
         return -1;
      }
   }
   else 
   {
      pr_info("Invalid input\n");
      return -1;
   }

   return len;
}

/* Allocate the shared profiler buffer and initialize all entries to -1. */
static bool allocatebuffer(void)
{
   gbuffer=vmalloc(buff_size);
   if(!gbuffer)
   {
      pr_info("vmalloc failed\n");
      return 0;
   }
   //initialized to -1
   memset(gbuffer,-1,buff_size);
   return 1;
}


/* Free the shared profiler buffer during module unload. */
static void freebuffer(void)
{
   if(!gbuffer) return;
   vfree(gbuffer);
   return;
}


/* Character-device open: intentionally empty for this MP. */
static int myopen(struct inode *inode, struct file *file)
{
   return 0;
}

/* Character-device close: intentionally empty for this MP. */
static int myclose(struct inode *inode, struct file *file)
{
   return 0;
}


/*
 * mmap callback for the char device.
 * Maps each page of the vmalloc'ed profiler buffer into the user VMA so the
 * monitor process can read samples directly from shared memory.
 */
static int mymmap(struct file *file, struct vm_area_struct *vma)
{
   unsigned long i=0;
   unsigned long usrbsize=vma->vm_end-vma->vm_start;
   if(usrbsize>buff_size) return -1;
   //Map each vmalloc'ed kernel page into the user VMA page by page
   for(i=0;i<usrbsize;i+=PAGE_SIZE)
   {
      unsigned long pfn=vmalloc_to_pfn((char*)gbuffer+i);
      unsigned long usraddr=vma->vm_start+i;
      if(remap_pfn_range(vma,usraddr,pfn,PAGE_SIZE,vma->vm_page_prot)!=0)
      {
         pr_info("remap_pfn_range failed\n");
         return -1;
      }
   }
   return 0;
}

static const struct file_operations myfops=
{
   .owner=THIS_MODULE,
   .open=myopen,
   .release=myclose,
   .mmap=mymmap,
};


/* Register the MP3 character device with major 423 and minor 0. */
static int chardvcini(void)
{
   int regnum;
   mp3dev=MKDEV(423,0);
   regnum=register_chrdev_region(mp3dev,1,"mp3chardev");
   if(regnum<0)
   {
      pr_info("egister_chrdev_region failed\n");
      return regnum;
   }
   cdev_init(&mp3cdev,&myfops);
   regnum=cdev_add(&mp3cdev,mp3dev,1);
   if(regnum<0) 
   {
      pr_info("cdev_add failed\n");
      unregister_chrdev_region(mp3dev,1);
      return regnum;
   }
   return 0;
}


/* Unregister the MP3 character device during module cleanup. */
static void chardvcexit(void)
{
   cdev_del(&mp3cdev);
   unregister_chrdev_region(mp3dev,1);
}


static const struct proc_ops my_ops=
{
   .proc_read=my_read,
   .proc_write=my_write,
};

/*
 * Module initialization.
 * Creates the proc entry, initializes the registration list and lock,
 * allocates the profiler buffer, creates the char device, and prepares the
 * delayed work item used for periodic sampling.
 */
static int __init mp3_init(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "MP3 MODULE LOADING\n");
   #endif
   // Insert your code here ...
   mp3dir=proc_mkdir("mp3",NULL);
   if(!mp3dir)
   {
      pr_info("proc_mkdir failed\n");
      return -1;
   }
   mp3status=proc_create("status",0666,mp3dir,&my_ops);
   if(!mp3status)
   {
      proc_remove(mp3dir);
      mp3dir=NULL;
      pr_info("proc_create failed\n");
      return -1;
   }
   mutex_init(&mp3mutex);
   INIT_LIST_HEAD(&mp3head);
   if(chardvcini()<0) return -1;
   if(!allocatebuffer()) return -1;
   wq=alloc_workqueue("mp3wq",WQ_UNBOUND|WQ_MEM_RECLAIM,1);
   if(!wq) return -1;
   INIT_DELAYED_WORK(&worker,workhandler);

   pr_info("MP3 MODULE LOADED\n");
   return 0;   
}


/*
 * Module cleanup.
 * Stops profiling, destroys the workqueue, frees shared memory, unregisters
 * the char device, clears the registration list, and removes proc entries.
 */
static void __exit mp3_exit(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
   #endif
   // Insert your code here ...

   cancel_delayed_work_sync(&worker);
   destroy_workqueue(wq);
   freebuffer();
   chardvcexit();
   cleanlist();
   mutex_destroy(&mp3mutex);
   if(mp3status) 
   {
      proc_remove(mp3status);
      mp3status=NULL;
   }
   if(mp3dir) 
   {
      proc_remove(mp3dir);
      mp3dir=NULL;
   }

   printk(KERN_ALERT "MP3 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp3_init);
module_exit(mp3_exit);