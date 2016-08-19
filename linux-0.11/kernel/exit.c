/*
 *  linux/kernel/exit.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <asm/segment.h>

int sys_pause(void);
int sys_close(int fd);

void release(struct task_struct * p)
{
	int i;

	if (!p)
		return;
	for (i=1 ; i<NR_TASKS ; i++)
		if (task[i]==p) {
			task[i]=NULL;
			free_page((long)p);
			schedule();
			return;
		}
	panic("trying to release non-existent task");
}

/* 
	向指定任务发送信号 sig ，权限为 priv 
	sig  - 信号值
	p    - 任务指针
	priv - 强制发送信号的标志。即不需要考虑进程用户属性或级别而能发送信号的权利。
 */
static inline int send_sig(long sig,struct task_struct * p,int priv)
{
	/* 如果任务指针为空或者信号不正确则退出 */
	if (!p || sig<1 || sig>32)
		return -EINVAL;

	/* #define suser() (current->euid == 0) */
	/* 
		如果强制发送标志置位,或者当前进程的有效用户标识符(euid)等于指定进程的euid
		或者当前进程是超级用户，则向进程 p 发送信号 sig, 即在进程 p 位图中添加该信号，
		否则出错退出
	 */
	if (priv || (current->euid==p->euid) || suser())
		p->signal |= (1<<(sig-1));
	else
		return -EPERM;
	return 0;
}

static void kill_session(void)
{
	struct task_struct **p = NR_TASKS + task;
	
	while (--p > &FIRST_TASK) {
		if (*p && (*p)->session == current->session)
			(*p)->signal |= 1<<(SIGHUP-1);
	}
}

/*
 * XXX need to check permissions needed to send signals to process
 * groups, etc. etc.  kill() permissions semantics are tricky!
 */
 /*
		系统调用 kill 可用于向任何进程或进程组发送任何信号，而并非只是杀死进程。
  */
int sys_kill(int pid,int sig)
{
	struct task_struct **p = NR_TASKS + task;
	int err, retval = 0;

	if (!pid) while (--p > &FIRST_TASK) {              /* pid == 0 信号发给当前进程的进程组中的所有进程 */
		if (*p && (*p)->pgrp == current->pid)          /* 当前进程组的 pgrp  和当前进程(进程组组长)的 pid 一样*/
			if ((err=send_sig(sig,*p,1)))
				retval = err;
	} else if (pid>0) while (--p > &FIRST_TASK) {      /* 发送给进程号是 pid 的进程 */
		if (*p && (*p)->pid == pid) 
			if ((err=send_sig(sig,*p,0)))
				retval = err;
	} else if (pid == -1) while (--p > &FIRST_TASK) {  /* 发送给除了第一个进程外的所有进程 */
		if ((err = send_sig(sig,*p,0)))
			retval = err;
	} else while (--p > &FIRST_TASK)                   /* 信号发送给进程组 -pid 的所有进程 */
		if (*p && (*p)->pgrp == -pid)
			if ((err = send_sig(sig,*p,0)))
				retval = err;
	return retval;
}

static void tell_father(int pid)
{
	int i;

	/* 向进程 pid 发送 SIGCHLD 信号 */
	if (pid)
		for (i=0;i<NR_TASKS;i++) {
			if (!task[i])
				continue;
			if (task[i]->pid != pid)
				continue;
			task[i]->signal |= (1<<(SIGCHLD-1));
			return;
		}
/* if we don't find any fathers, we just release ourselves */
/* This is not really OK. Must change it to make father 1 */
	printk("BAD BAD - no father found\n\r");
	release(current);
}

int do_exit(long code)
{
	int i;
	/* 释放当前进程代码段和数据段所占的内存页 */
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));

	/* 把当前进程的子进程的父进程设置为 init 进程，如果有子进程是僵尸进程，向 init 进程发送 SIGCHLD 信号 */
	for (i=0 ; i<NR_TASKS ; i++)
		if (task[i] && task[i]->father == current->pid) {
			task[i]->father = 1;
			if (task[i]->state == TASK_ZOMBIE)
				/* assumption task[1] is always init */
				(void) send_sig(SIGCHLD, task[1], 1);
		}

	/* 关闭当前进程打开的所有文件 */
	for (i=0 ; i<NR_OPEN ; i++)
		if (current->filp[i])
			sys_close(i);

	/* 对当前进程工作目录 pwd、根目录 root 以及执行程序文件的 i 节点进行同步操作，放回各个 i 节点并分别置空（释放） */
	iput(current->pwd);
	current->pwd=NULL;
	iput(current->root);
	current->root=NULL;
	iput(current->executable);
	current->executable=NULL;

	/* 如果当前进程是会话头领，并且其有控制终端，则释放该终端 */
	if (current->leader && current->tty >= 0)
		tty_table[current->tty].pgrp = 0;
	/* 如果当前进程上次使用过协处理器，则将 last_task_used_math 置空 */
	if (last_task_used_math == current)
		last_task_used_math = NULL;

	/* 如果当前进程是 Leader 进程，则终止该会话的所有相关进程 */
	if (current->leader)
		kill_session();

	/* 把当前进程置为僵尸进程，表明当前进程已经释放了资源。并保存将由父进程读取的退出码 */
	current->state = TASK_ZOMBIE;
	current->exit_code = code;

	/* 通知父进程 */
	tell_father(current->father);

	/* 重新调度 */
	schedule();
	return (-1);	/* just to suppress warnings */
}

int sys_exit(int error_code)
{
	return do_exit((error_code&0xff)<<8);
}

int sys_waitpid(pid_t pid,unsigned long * stat_addr, int options)
{
	int flag, code;
	struct task_struct ** p;

	verify_area(stat_addr,4);
repeat:
	flag=0;
	/* 
		如果 pid > 0, 找出当前进程的子进程 
		如果 pid = 0, 找出当前进程同组的进程
		如果 pid < -1, 找出进程组 -pid 的进程
		如果 pid = -1, 等待任何子进程
	 */
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p || *p == current)
			continue;
		if ((*p)->father != current->pid)
			continue;
		if (pid>0) {
			if ((*p)->pid != pid)
				continue;
		} else if (!pid) {
			if ((*p)->pgrp != current->pgrp)
				continue;
		} else if (pid != -1) {
			if ((*p)->pgrp != -pid)
				continue;
		}
		switch ((*p)->state) {
			case TASK_STOPPED:
				if (!(options & WUNTRACED)) /* WUNTRACED 遇到 TASK_STOPPED 直接返回*/
					continue;
				put_fs_long(0x7f,stat_addr);
				return (*p)->pid;
			case TASK_ZOMBIE:          /* wait 到僵尸进程就释放，同时返回子进程的 pid */
				current->cutime += (*p)->utime;
				current->cstime += (*p)->stime;
				flag = (*p)->pid;
				code = (*p)->exit_code;
				release(*p);
				put_fs_long(code,stat_addr);
				return flag;
			default:
				flag=1;
				continue;
		}
	}
	/* 等待的进程处于运行态、就绪态或者睡眠态，flag = 1 */
	if (flag) {
		if (options & WNOHANG) /* 如果有 WNOHANG 选项，直接返回 0 不挂起 */
			return 0;
		current->state=TASK_INTERRUPTIBLE; /* 当前进程置为可被中断的睡眠状态 */
		schedule(); /* 执行调度 */
		if (!(current->signal &= ~(1<<(SIGCHLD-1)))) /* 如果没有收到除 SIGCHLD 以外的信号，接着继续等待，否则返回错误 */
			goto repeat;
		else
			return -EINTR;
	}
	return -ECHILD;
}


