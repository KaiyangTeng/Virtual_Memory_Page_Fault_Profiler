````md
# UIUC CS 423 MP3: Virtual Memory Page Fault Profiler

## 1. Project Overview

This project implements a Linux kernel module that profiles the virtual memory behavior of user-space processes. The profiler tracks registered processes, periodically samples their page fault counts and CPU utilization, stores the results in an in-kernel buffer, and exposes that buffer to user space through a character device using `mmap()`.

The goal of this project is to study how memory access patterns and multiprogramming affect page faults, CPU utilization, and system performance.

## 2. Implementation

The kernel module provides a proc filesystem interface at:

```text
/proc/mp3/status
````

User processes register and unregister themselves by writing commands to this file:

```text
R <PID>
U <PID>
```

The module maintains a linked list of registered PIDs protected by a mutex. When the first process is registered, the profiler starts a delayed workqueue. When the last process unregisters, the delayed work is cancelled.

The profiler buffer is allocated with `vmalloc()` because it needs to be virtually contiguous but not physically contiguous. The buffer is initialized to `-1`, which is the sentinel value expected by the monitor program.

Every 50 ms, the delayed work handler samples all registered processes using the provided `get_cpu_use()` helper. For each sampling interval, it accumulates:

```text
jiffies
soft page faults
hard page faults
CPU utilization = utime + stime
```

Each sample is written into the shared buffer as four `unsigned long` values.

To expose the buffer to user space, the module registers a character device with major number `423` and minor number `0`. The `mmap()` callback maps each page of the `vmalloc()` buffer into the monitor process by using `vmalloc_to_pfn()` and `remap_pfn_range()`. This allows the monitor to read profiling data directly from shared memory without repeated kernel-to-user copies.

## 3. Running the Profiler

Build the kernel module:

```bash
make
```

Load the module:

```bash
sudo insmod mp3.ko
```

Create the character device node:

```bash
sudo mknod node c 423 0
```

Run workloads, for example:

```bash
nice ./work 1024 R 50000 &
nice ./work 1024 R 10000 &
```

Collect the profiling data:

```bash
./monitor > profile.data
```

Unload the module:

```bash
sudo rmmod mp3
```

## 4. Case Study 1: Thrashing and Locality

The first experiment compares two random-access workloads:

```bash
nice ./work 1024 R 50000 &
nice ./work 1024 R 10000 &
```

<p align="center">
  <img src="plots/case_1_work_1_2.png" width="650"/>
</p>

Both processes allocate a large 1024 MB memory region and access it randomly. Since random access has poor locality, the processes frequently touch pages that are not resident in memory, causing the accumulated page fault count to rise steadily. The workload with 50,000 accesses per iteration creates more memory pressure than the workload with 10,000 accesses per iteration.

The second experiment compares random access with locality-based access:

```bash
nice ./work 1024 R 50000 &
nice ./work 1024 L 10000 &
```

<p align="center">
  <img src="plots/case_1_work_3_4.png" width="650"/>
</p>

Compared with the fully random-access experiment, the locality-based workload produces fewer page faults. This is because nearby memory locations are reused more often, so pages brought into memory are more likely to be accessed again before eviction. The result confirms that locality directly reduces page fault pressure and improves virtual memory performance.

## 5. Case Study 2: Multiprogramming

This experiment runs `N` copies of the workload:

```bash
./work 200 R 10000
```

with:

```text
N = 5, 11, 16, 20, 22
```

<p align="center">
  <img src="plots/case_2.png" width="650"/>
</p>

As `N` increases, total CPU utilization initially increases because more processes are available to run concurrently. However, at high values of `N`, the combined working set becomes larger than available physical memory. Since each process allocates 200 MB, the system eventually experiences heavy page replacement and swap activity.

The sharp increase at high `N` indicates that completion time becomes much longer under memory pressure. Because the graph reports total accumulated utilization, longer execution under thrashing leads to a much larger total value. This shows that multiprogramming improves utilization only up to the point where memory pressure becomes the bottleneck.

## 6. Conclusion

This project demonstrates how procfs, kernel linked lists, delayed workqueues, `vmalloc()`, character devices, and `mmap()` can be combined to build a lightweight kernel-level profiler.

The experimental results show that random access causes more page faults than locality-based access, and that excessive multiprogramming can lead to memory pressure and thrashing. Overall, the profiler provides a clear view of how virtual memory behavior affects application performance.

```
```
