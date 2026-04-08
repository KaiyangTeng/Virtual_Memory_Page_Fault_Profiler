#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
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

static void workhandler(struct work_struct *worker)
{
   unsigned long timesum=jiffies,minfault=0,majfault=0,cpu=0;
   struct listnode *curr;
   bool empty=true;
   mutex_lock(&mp3mutex);
   list_for_each_entry(curr,&mp3head,node)
   {
      unsigned long minf,majf,ut,st;
      if(get_cpu_use(curr->pid,&minf,&majf,&ut,&st)==0)
      {
         empty=false;
         minfault+=minf;
         majfault+=majf;
         cpu+=ut+st;
      }
   }
   mutex_unlock(&mp3mutex);
   if(!empty) 
   {
      gbuffer[indx++]=timesum;
      gbuffer[indx++]=minfault;
      gbuffer[indx++]=majfault;
      gbuffer[indx++]=cpu;
      if(indx>=48000) indx=0;
      mod_delayed_work(wq,&worker,msecs_to_jiffies(50));
   }
}

bool taskregister(pid_t pid)
{
   struct listnode *temp;
   struct listnode *curr=kmalloc(sizeof(*curr),GFP_KERNEL);
   if(!curr) 
   {
      pr_info("reg kmalloc failed\n");
      return 0;
   }
   curr->pid=pid;
   mutex_lock(&mp3mutex);
   list_for_each_entry(temp,&mp3head,node)
   {
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
   return 1;
}


bool deregister(pid_t pid)
{
   struct listnode *curr,*temp;
   mutex_lock(&mp3mutex);
   list_for_each_entry_safe(curr,temp,&mp3head,node)
   {
      if(curr->pid==pid)
      {
         list_del(&curr->node);
         kfree(curr);
         mutex_unlock(&mp3mutex);
         return 1;
      }
   }
   mutex_unlock(&mp3mutex);
   return 0;
}


void cleanlist()
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


ssize_t my_read(struct file * inputfile, char __user * usrbuf, size_t len, loff_t * p)
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
		return -EFAULT;
	}
   *p+=len;
   kfree(kbuf);
	return len;
}


ssize_t my_write(struct file * inputfile, const char __user *usrbuf, size_t len, loff_t * p)
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
   if(kbuf[0]=='R')
   {
      int pid;
      if(sscanf(kbuf,"R %d",&pid)==1)
      {
         struct task_struct*linux_task=find_task_by_pid(pid);
         if(!linux_task) 
			{
				pr_info("linux_task not found\n");
				return -1;
			}
         if(!taskregister(pid)) 
			{
				pr_info("taskregister failed\n");
				return -1;
			}
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
         if(!deregister(pid))
			{
				pr_info("deregister failed\n");
				return -1;
			}
      }
      else 
      {
         pr_info("Bad input R\n");
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

bool allocatebuffer(void)
{
   unsigned long i=0;
   gbuff=vmalloc(buff_size);
   if(!gbuffer)
   {
      pr_info("vmalloc failed\n");
		return 0;
   }
   memset(gbuff,-1,sizeof(gubff));
   for(i=0;i<buff_size;i+=PAGE_SIZE)
   {
      struct page *curr=vmalloc_to_page(gbuffer+i);
      SetPageReserved(curr);
   }
   return 1;
}

void freebuffer(void)
{
   if(!gbuffer) return;
   
   for(i=0;i<buff_size;i+=PAGE_SIZE)
   {
      struct page *curr=vmalloc_to_page(gbuffer+i);
      ClearPageReserved(curr);
   }
   vfree(gbuffer);
   return;
}

static int chardvcini(void)
{
   int regnum;
   mp3dev=MKDEV(MP3_MAJOR,MP3_MINOR);
   regnum=register_chrdev_region(mp3dev,MP3_DEV_COUNT,MP3_DEV_NAME);
   if(regnum<0)
   {
      pr_info("egister_chrdev_region failed\n");
      return regnum;
   }
   cdev_init(&mp3cdev, &myfops);
   regnum=cdev_add(&mp3cdev,mp3_dev,MP3_DEV_COUNT);
   if(regnum<0) 
   {
      pr_info("cdev_add failed\n");
      unregister_chrdev_region(mp3dev,MP3_DEV_COUNT);
      return regnum;
   }
   return 0;
}

static void chardvcexit(void)
{
   cdev_del(&mp3_cdev);
   unregister_chrdev_region(mp3_dev,MP3_DEV_COUNT);
}


static const struct proc_ops my_ops=
{
	.proc_read=my_read,
	.proc_write=my_write,
};

// mp1_init - Called when module is loaded
int __init mp3_init(void)
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

   pr_info("MP3 MODULE LOADED\n");
   return 0;   
}

// mp1_exit - Called when module is unloaded
void __exit mp3_exit(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
   #endif
   // Insert your code here ...
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
