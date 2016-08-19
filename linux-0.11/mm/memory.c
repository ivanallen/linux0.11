/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

volatile void do_exit(long code);

static inline volatile void oom(void)
{
	printk("out of memory\n\r");
	do_exit(SIGSEGV);
}

#define invalidate() \
__asm__("movl %%eax,%%cr3"::"a" (0))

/* these are not to be changed without changing head.s etc */
#define LOW_MEM 0x100000
#define PAGING_MEMORY (15*1024*1024)
#define PAGING_PAGES (PAGING_MEMORY>>12)
#define MAP_NR(addr) (((addr)-LOW_MEM)>>12)
#define USED 100

#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

static long HIGH_MEMORY = 0;

#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024))

static unsigned char mem_map [ PAGING_PAGES ] = {0,};

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 * 从位图 mem_map 末端开始向前扫描所有页面标志，若有空闲页面(标记为0),返回页面物理地址起始位置
 * 参数：%1:ax = 0, %2:LOW_MEM管理的物理地址起始位置 %3：cx = 总的页数 %4: edi=位图最后一个元素的地址。
 * 输出：eax = 物理地址起始地址。
 * scasb: SCAN String Byte, 就是计算 cmp al, byte ptr [es:edi],如果DF=0，完成后edi自增1.
 * repne: 表示把后面的指令最多执行 ecx 次，直到 ZF=1 为止就不再执行
 */
unsigned long get_free_page(void)
{
register unsigned long __res asm("ax");

__asm__("std ; repne ; scasb\n\t"    /* 找到一个不为0的字节 */
	"jne 1f\n\t"                     /* 如果没找到就结束, 如果找到了，memmap[edi+1]就是要找的位置，标记为 1 */
	"movb $1,1(%%edi)\n\t"           /* ecx 是表示第 ecx 个页面是空闲的 */
	"sall $12,%%ecx\n\t"             /* ecx = ecx<<12，计算出相对偏移 */
	"addl %2,%%ecx\n\t"              /* ecx = ecx + LOWMEM，计算实际物理地址偏移 */
	"movl %%ecx,%%edx\n\t"           /* 把物理地址存放到 edx */
	"movl $1024,%%ecx\n\t"           /* ecx = 1024 */
	"leal 4092(%%edx),%%edi\n\t"     /* edi = edx + 4092 */
	"rep ; stosl\n\t"                /* rep mov dword ptr [es:edi], al */
	"movl %%edx,%%eax\n"             /* eax = edx，返回实际物理地址 */
	"1:"
	:"=a" (__res)
	:"0" (0),"i" (LOW_MEM),"c" (PAGING_PAGES),
	"D" (mem_map+PAGING_PAGES-1)
	);
return __res;
}

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
void free_page(unsigned long addr)
{
	if (addr < LOW_MEM) return;
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
	addr -= LOW_MEM;
	addr >>= 12;
	if (mem_map[addr]--) return;
	mem_map[addr]=0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
int free_page_tables(unsigned long from,unsigned long size)
{
	unsigned long *pg_table;
	unsigned long * dir, nr;

	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
	size = (size + 0x3fffff) >> 22;
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	for ( ; size-->0 ; dir++) {
		if (!(1 & *dir))
			continue;
		pg_table = (unsigned long *) (0xfffff000 & *dir);
		for (nr=0 ; nr<1024 ; nr++) {
			if (1 & *pg_table)
				free_page(0xfffff000 & *pg_table);
			*pg_table = 0;
			pg_table++;
		}
		free_page(0xfffff000 & *dir);
		*dir = 0;
	}
	invalidate();
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 * 好了，现在是内存管理 mm 中最为复杂的程序之一。它通过只复制内存页面来拷贝一定范围内
 * 线性地址中的内容。希望代码中没有错误，因为我不想再调试这块代码了:-)
 * 
 * 注意！我们并不复制任何内存块 - 内存块地址需要是 4MB(正好是一个页目录项对应的内存长度)，
 * 因为这样处理可以使函数很简单。不管怎样，它仅被 fork() 使用。
 *
 * 注意2！！当 from == 0时，说明是在为第一次 fork() 调用复制内核空间。此时我们就不想复制
 * 整个页目录项对应的内存，因为这样做会导致内存严重浪费 - 我们只须复制开头的 160 个页面 - 对应
 * 640KB。 即使是复制这些页面也已经超出了我们的需求，但这不会占用更多的内存 - 在低 1MB 内存
 * 范围内我们不执行写时复制操作，所以这些页面可以与内核共享。因此这是 nr = xxxx 的特殊情况
 * （nr在程序中指页面数）。
 * 复制指定线性地址和长度内在对应的页目录项和页表项，从而被复制的页目录和页表对应的原物理
 * 内存页面区被两套页表映射而共享使用。复制时，需申请新的页面来存放新页表，原物理内存区将被共享。
 * 此后两个进程（父进程和其子进程）将共享内在区，直到有一个进程执行写操作时，内核才会为写操
 * 作进程分配新的内存页（写时复制机制）。
 * 参数：from/to 是线性地址，size 是需要复制（共享）的内存长度。单位是字节。
 */
int copy_page_tables(unsigned long from,unsigned long to,long size)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long nr;

	if ((from&0x3fffff) || (to&0x3fffff)) /* 如果 from 和 to 不是 4MB 对齐就死机 */
		panic("copy_page_tables called with wrong alignment");
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* 这句相当于 (from >> 22) << 2。右移22位再移2位，拿到页目录索引号偏移地址。 */
	to_dir = (unsigned long *) ((to>>20) & 0xffc); /* 同样拿到页目录索引号的偏移地址 */
	size = ((unsigned) (size+0x3fffff)) >> 22; /* 计算需要几个 4MB 内存页 */
	for( ; size-->0 ; from_dir++,to_dir++) {
		if (1 & *to_dir) /* 如果目的目录项指定的页表已经存在就死机 */
			panic("copy_page_tables: already exist");
		if (!(1 & *from_dir)) /* 如果源目录项不存在就继续处理下一个目录项 */
			continue;
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir); /* 计算源目录项对应的页表基址 */
		if (!(to_page_table = (unsigned long *) get_free_page())) /* 为目的目录项分配对应的页表 */
			return -1;	/* Out of memory, see freeing */
		*to_dir = ((unsigned long) to_page_table) | 7; /* 将目的页页表基址装入目的目录项 */
		nr = (from==0)?0xA0:1024; /* 如果在内核空间，则仅需要复制头160页的页表项。否则需要复制所有的1024个页表项。 */
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) {
			this_page = *from_page_table; /* 拿到页基址 */
			if (!(1 & this_page)) /* 如果c这个页不在内存，继续找下一个页表项 */
				continue;
			this_page &= ~2; /* 去掉这个页的读写属性，禁止读写 */
			*to_page_table = this_page; /* 复制到目的页表中 */
			if (this_page > LOW_MEM) { /* 如果该页在1MB以上的地址，需要设置数组mem_map的引用计数，增加引用次数 */
				*from_page_table = this_page; /* 让源页也只读 */
				this_page -= LOW_MEM;
				this_page >>= 12;
				mem_map[this_page]++; /* 增加引用计数 */
			}
		}
	}
	invalidate(); /* 刷新页面变换高速缓冲 */
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 * 把线性地址 address 映射到 page 处
 */
unsigned long put_page(unsigned long page,unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */
	/* 先判断下 page 的有效性 */
	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);

	/* 取到页目录偏移 */
	page_table = (unsigned long *) ((address>>20) & 0xffc);

	/* 查看是否有对应的页表，如果有，取到页表偏移 */
	if ((*page_table)&1)
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	/* 如果没有对应的页表，申请一页内存，做页表，并将该页表偏移置入页目录。 */
	else {
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp|7;
		page_table = (unsigned long *) tmp;
	}
	/* 在该页表中置入物理页偏移地址 */
	page_table[(address>>12) & 0x3ff] = page | 7;
/* no need for invalidate 不刷新页机构高速缓冲*/
	return page;
}

void un_wp_page(unsigned long * table_entry)
{
	unsigned long old_page,new_page;

	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) {
		*table_entry |= 2;
		invalidate();
		return;
	}
	if (!(new_page=get_free_page()))
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	*table_entry = new_page | 7;
	invalidate();
	copy_page(old_page,new_page);
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */
void do_wp_page(unsigned long error_code,unsigned long address)
{
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
	un_wp_page((unsigned long *)
		(((address>>10) & 0xffc) + (0xfffff000 &
		*((unsigned long *) ((address>>20) &0xffc)))));

}

void write_verify(unsigned long address)
{
	unsigned long page;

	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return;
	page &= 0xfffff000;
	page += ((address>>10) & 0xffc);
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);
	return;
}

void get_empty_page(unsigned long address)
{
	unsigned long tmp;
	/* 申请一页内存， 并作映射*/
	if (!(tmp=get_free_page()) || !put_page(tmp,address)) {
		free_page(tmp);		/* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable.
 * try_to_share() 在任务 p 中检查位于 address 处的页面，看页面是否存在。
 * 如果是干净的话（没有被修改过），就与当前任务共享。
 * 注意！这里我们已经假定 p != current，并且它们共享同一个执行程序。
 */

static int try_to_share(unsigned long address, struct task_struct * p)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

	from_page = to_page = ((address>>20) & 0xffc); 
	from_page += ((p->start_code>>20) & 0xffc); /* 计算进程 p 中在 address 处的目录项偏移 */
	to_page += ((current->start_code>>20) & 0xffc); /* 计算当前进程中在 address 处的目录项偏移 */
/* is there a page-directory at from? */
	from = *(unsigned long *) from_page; /* 取页表偏移 */
	if (!(from & 1))
		return 0;
	from &= 0xfffff000;
	from_page = from + ((address>>10) & 0xffc); /* 计算页表项偏移 */
	phys_addr = *(unsigned long *) from_page; /* 计算页偏移 */
/* is the page clean and present? 物理页干净并且存在吗？*/
/*                              0 1 0 0 0 0 0 1
	|--------------------|---|-|-|-|-|-|-|-|-|-|
	|    页表物理基址    |AVL|G|P|D|A|P|P|U|R|P|
	                           |A|   |C|W|S|W|
	                           |T|   |D|T|
	D: Dirty, 表示是否写过数据
	P: present, 表示是否存在

 */
	if ((phys_addr & 0x41) != 0x01)
		return 0;
	phys_addr &= 0xfffff000; /* 物理页地址 */
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
		return 0;
	to = *(unsigned long *) to_page; /* 取页表基址 */
	if (!(to & 1)) { /* 如果页表不存在就申请一页 */
		if ((to = get_free_page()))
			*(unsigned long *) to_page = to | 7;
		else
			oom();
	}
	to &= 0xfffff000;
	to_page = to + ((address>>10) & 0xffc); /* 计算页表项偏移 */
	if (1 & *(unsigned long *) to_page) /* 取物理页，如果物理页基址，如果物理页存在就终止程序 */
		panic("try_to_share: to_page already exists"); 
/* share them: write-protect */
	*(unsigned long *) from_page &= ~2; /* 把读写属性关掉 */
	*(unsigned long *) to_page = *(unsigned long *) from_page; /* 把页表项内容复制过去 */
	invalidate(); /* 刷新页机构 */
	phys_addr -= LOW_MEM;
	phys_addr >>= 12;
	mem_map[phys_addr]++; /* 增加页位图引用计数 */
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 * share_page() 试图找到一个进程，它可以与当前进程共享页面。参数 address 是
 * 当前进程数据空间中期望共享的某页面地址。注意这里的 address 传进来的是逻辑地址
 * 
 * 首先我们通过检测 executable->i_count 来查证是否可行。如果有其他任务已共享该 inode, 则它应该大于1.
 */
static int share_page(unsigned long address)
{
	struct task_struct ** p;

	if (!current->executable)
		return 0;
	if (current->executable->i_count < 2) /* 如果只有一个进程在用这个可执行体，就返回 0 */
		return 0;
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p)
			continue;
		if (current == *p)
			continue;
		if ((*p)->executable != current->executable)
			continue;
		if (try_to_share(address,*p))
			return 1;
	}
	return 0;
}

/* 
	执行缺页处理，这个函数由 page.s 中的 page_fault 函数调用 
	该函数首先尝试与已加载的相同文件进行页面共享，或者只是由
	于进程动态申请内在页面而只需映射一页物理内存面即可。若共
	享操作不成功，那么只能从相应文件中读入所缺的数据页面到指
	定线性地址处。
	address 是产生异常页面的线性地址
 */
void do_no_page(unsigned long error_code,unsigned long address)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;

	address &= 0xfffff000; /* 取得线性地址所在的页面基址 */
	tmp = address - current->start_code; /* 缺页页面对应的逻辑地址，就是减掉段基址后的地址 */

	/* 没有可执行体的进程或者逻辑地址超出代码+数据的长度时，需要申请一页物理内存。并映射到指定的线性地址 */
	if (!current->executable || tmp >= current->end_data) {
		get_empty_page(address);
		return;
	}
	if (share_page(tmp)) /* 如果不能共享，继续 */
		return;
	if (!(page = get_free_page())) /* 申请一个物理页 */
		oom();
/* remember that 1 block is used for header */
/* 
	程序头需要使用一个数据块。在读文件时，需要跳过第一块数据。
	先计算缺页所在的数据块号。因为每块数据长度为 BLOCK_SIZE = 1KB,因此一页内存可以存放 4 个数据块。
	进程逻辑地址 tmp 除以数据块的大小再加 1 即可得出缺少的页面在执行映像文件中的起始块号 block.根据这个
	块号和执行文件的 i 节点，我们就可以从映射位图中找到对应块设备中的对应的设备块号（保存在nr[]数组中）。
	利用 bread_page() 即可把这 4 个逻辑块读入到物理页面 page 中。
 */
	block = 1 + tmp/BLOCK_SIZE; /* 计算起始块号 */
	for (i=0 ; i<4 ; block++,i++)
		nr[i] = bmap(current->executable,block);  /* 设备上对应的逻辑块号 */
	bread_page(page,current->executable->i_dev,nr); /* 读设备上 4 个逻辑块放到刚申请的 page 中 */

	/* 
		在读设备逻辑块操作时，可能会出现这样一种情况，即在执行文件中的读取页面位置可能离文件尾不到 1 个页面的
		长度。因此就可能读入一些无用的信息。下面的操作就是把这部分超出执行文件 end_data 以后的部分清零处理。
	 */
	i = tmp + 4096 - current->end_data;
	tmp = page + 4096;
	while (i-- > 0) {
		tmp--;
		*(char *)tmp = 0;
	}
	if (put_page(page,address))
		return;
	free_page(page);
	oom();
}

void mem_init(long start_mem, long end_mem)
{
	/*
	 * 这个函数把 1MB - start_mem 的内存页标记为已使用，start_mem - end_mem 的内存页标记为未使用
	 */
	int i;
	/* HIGH_MEMORY = 0x01000000 = 16MB 定义在前面的一个静态变量*/
	HIGH_MEMORY = end_mem;

	/* 
	 * mem_map 前面定义的一个内存位图，USED=100 ，表示已经使用 
	 * 所有页初始化为已经使用
	 * PAGING_PAGES = 3840 = 15MB/4096B
	 */
	for (i=0 ; i<PAGING_PAGES ; i++)
		mem_map[i] = USED;


	/* 
	 * #define MAP_NR(addr) (((addr)-LOW_MEM)>>12) 
	 * i = (start_mem-LOW_MEM)/2^12
	 * i = (4MB-1MB)/2^12 = 3MB/2^12 = 3*2^8 = 768
	 * end_mem = 16MB - 4MB = 12MB
	 * end_mem = end_mem/2^12 = 12 * 2^8 = 3072
	 */
	i = MAP_NR(start_mem);
	end_mem -= start_mem;
	end_mem >>= 12;
	while (end_mem-->0)
		mem_map[i++]=0;
}

void calc_mem(void)
{
	int i,j,k,free=0;
	long * pg_tbl;

	for(i=0 ; i<PAGING_PAGES ; i++)
		if (!mem_map[i]) free++;
	printk("%d pages free (of %d)\n\r",free,PAGING_PAGES);
	for(i=2 ; i<1024 ; i++) {
		if (1&pg_dir[i]) {
			pg_tbl=(long *) (0xfffff000 & pg_dir[i]);
			for(j=k=0 ; j<1024 ; j++)
				if (pg_tbl[j]&1)
					k++;
			printk("Pg-dir[%d] uses %d pages\n",i,k);
		}
	}
}
