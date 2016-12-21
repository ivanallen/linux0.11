/*
 *  linux/kernel/signal.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <signal.h>

volatile void do_exit(int error_code);

int sys_sgetmask()
{
	return current->blocked;
}

int sys_ssetmask(int newmask)
{
	int old=current->blocked;

	current->blocked = newmask & ~(1<<(SIGKILL-1));
	return old;
}

static inline void save_old(char * from,char * to)
{
	int i;

	verify_area(to, sizeof(struct sigaction));
	for (i=0 ; i< sizeof(struct sigaction) ; i++) {
		put_fs_byte(*from,to);
		from++;
		to++;
	}
}

static inline void get_new(char * from,char * to)
{
	int i;

	for (i=0 ; i< sizeof(struct sigaction) ; i++)
		*(to++) = get_fs_byte(from++);
}


/*restorer 参数由 libc 提供*/
int sys_signal(int signum, long handler, long restorer)
{
	struct sigaction tmp;

	if (signum<1 || signum>32 || signum==SIGKILL)
		return -1;
	tmp.sa_handler = (void (*)(int)) handler;
	tmp.sa_mask = 0;
	tmp.sa_flags = SA_ONESHOT | SA_NOMASK;
	tmp.sa_restorer = (void (*)(void)) restorer;
	handler = (long) current->sigaction[signum-1].sa_handler;
	current->sigaction[signum-1] = tmp;
	return handler;
}

int sys_sigaction(int signum, const struct sigaction * action,
	struct sigaction * oldaction)
{
	struct sigaction tmp;

	if (signum<1 || signum>32 || signum==SIGKILL)
		return -1;
	tmp = current->sigaction[signum-1];
	get_new((char *) action,
		(char *) (signum-1+current->sigaction));
	if (oldaction)
		save_old((char *) &tmp,(char *) oldaction);
	if (current->sigaction[signum-1].sa_flags & SA_NOMASK)
		current->sigaction[signum-1].sa_mask = 0;
	else
		current->sigaction[signum-1].sa_mask |= (1<<(signum-1));
	return 0;
}

/*
	内核栈的样子：
	|ret addr| <- esp[0]             esp + 0
	|  SIGN  | <- esp[1]             esp + 4
	|   EAX  | <- esp[2]             esp + 8
	|   EBX  | <- esp[3]             esp + c
	|   ECX  | <- esp[4]             esp + 10
	|   EDX  | <- esp[5]             esp + 14
	|   FS   | <- esp[6]             esp + 18     这些都是用户态的选择子，先保存到内核栈里
	|   ES   | <- esp[7]             esp + 1c
	|   DS   | <- esp[8]             esp + 20
	|   EIP  | <- esp[9]             esp + 24     int 80 后面那条指令，，当然也可能是别的中断后面那条。这个值很有可能在 do_signal 中被修改为信号处理函数的入口
	|   CS   | <- esp[10]            esp + 28
	| EFLAGS | <- esp[11]            esp + 2c
	|   ESP  | <- esp[12]            esp + 30     这个值可能在后面被 do_signal 中更改。
	|   SS   | <- esp[13]            esp + 34
	|   ...  | <- esp[14]            esp + 38

	                图 1
 */
void do_signal(long signr,long eax, long ebx, long ecx, long edx,
	long fs, long es, long ds,
	long eip, long cs, long eflags,
	unsigned long * esp, long ss)
{
	unsigned long sa_handler;
	long old_eip=eip;
	/* 
		struct sigaction {
			void (*sa_handler)(int);
			sigset_t sa_mask;
			int sa_flags;
			void (*sa_restorer)(void); 清理函数由 libc 提供
		};
		current中的 sigaction 是一个类型为 struct sigaction 的大小为32的数组。
		下面这句是找到当前信号对应的 sigaction
		sa = current->sigaction[signr-1]
	 */
	struct sigaction * sa = current->sigaction + signr - 1;
	int longs;
	unsigned long * tmp_esp;

	/* 拿到信号处理函数 */
	sa_handler = (unsigned long) sa->sa_handler;

	/* 如果信号处理函数为 SIG_IGN(值为1)，直接返回。*/
	if (sa_handler==1)
		return;
	if (!sa_handler) { /* 如果信号处理函数值为 0 */
		if (signr==SIGCHLD)      /* 如果该信号是 SIGCHLD，直接返回，否则终止进程 */
			return;
		else
			do_exit(1<<(signr-1)); /* 这个参数可以利用 wait 或者 waitpid 取得，以取得子进程的退出状态码 */
	}
	if (sa->sa_flags & SA_ONESHOT)  /* 如果该信号只需要使用一次，就将该句柄置空 */ 
		sa->sa_handler = NULL;


/*
	用户栈：
	|  old  | <-esp     旧的用户栈的栈顶(通过 iret 返回到用户态时用户栈的样子)
	|  ???  | <-esp + 4

 */

	/* 把返回到用户态的 eip 指针改为该函数，注意，这里是直接修改了栈上那块内存，
	   这样的话 iret 就无法返回到进入内核态前的样子了，而是 iret 到了 sa_handler 这个信号处理函数里。
	   所以这里还必需要手工构造 sa_handler 函数的运行栈空间。
	*/
	*(&eip) = sa_handler; 

	longs = (sa->sa_flags & SA_NOMASK)?7:8; 
	*(&esp) -= longs;        /* 将原用户堆栈指针 esp 向下扩展 7 或者 8 个 4 字节(用来存放调用信号句柄的参数等) */

/*
	这样修改后，当从 iret 返回后，用户栈的栈顶会变成这样子。也就是为 sa_handler 函数手工构造了一个栈空间
	用户栈：
	
	|  ???  | <-esp + 0     sa_restorer 该值由 libc 函数库提供。
	|  ???  | <-esp + 4     signr
	|  ???  | <-esp + 8     eax
	|  ???  | <-esp + c     ecx
	|  ???  | <-esp + 10    edx
	|  ???  | <-esp + 14    eflags
	|  ???  | <-esp + 18    这里需要填写 ret 地址，也就是旧的 eip
	|  ???  | <-esp + 1c    旧的用户栈的栈顶
	|  old  | <-esp + 20

 */

	verify_area(esp,longs*4); /* 检查页面是否有效，是否能够写入数据 */
	tmp_esp=esp;
	put_fs_long((long) sa->sa_restorer,tmp_esp++);  /* 在用户堆栈中从上到下(地址有低到高)存放 sa_restorer,signr,blocked(如果SA_NOMASK置位)，eax,ecx,edx,eflags和用户程序原 eip */
	put_fs_long(signr,tmp_esp++);
	if (!(sa->sa_flags & SA_NOMASK))
		put_fs_long(current->blocked,tmp_esp++);
	put_fs_long(eax,tmp_esp++);
	put_fs_long(ecx,tmp_esp++);
	put_fs_long(edx,tmp_esp++);
	put_fs_long(eflags,tmp_esp++);
	put_fs_long(old_eip,tmp_esp++);          /* 存放旧的 eip */
	current->blocked |= sa->sa_mask;

/* 从这里 iret 切换出去后，将到达下面这个栈，同时进入信号处理函数。信号处理函数执行完后，才会到达以前 int 指令的下一句*/

/*
	用户栈：
	
	|sa_rest| <-esp + 0   返回到清理函数的地址，该函数地址由 libc 函数库提供。
	| signr | <-esp + 4   参数1，信号值
	|blocked| <-esp + 8   <--  如果有这个的话
	|  eax  | <-esp + c
	|  ecx  | <-esp + 10
	|  edx  | <-esp + 14
	|eflags | <-esp + 18
	|old_eip| <-esp + 1c    本来应该返回的地址
	|  ???  | <-esp + 20    旧的用户栈的栈顶，信号处理函数返回后，将转到 int 80 后面那条指令，当然也可能是别的中断后面那条。
	|  old  | <-esp + 24



    sa_restorer:   没有block 的版本
      add esp, 4 丢弃 signr
      pop eax
      pop ecx
      pop edx
      popfd
      ret

    sa_restorer:
      add esp, 4
      call __ssetmask
      add esp, 4 丢弃 block
      pop eax
      pop ecx
      pop edx
      popfd
      ret
 */
}


/*
void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000;
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}
*/