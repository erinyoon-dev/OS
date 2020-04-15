/**
  * This file is for Project 3: LWP Implementation.
  *
  * @author   Hanyang Univ. JinMyeong Lee (jinious1111@naver.com)
  * @since    2018-05-07
*/

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

extern struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

/**
  * This function is used to create thread.
  * allocate thread through allocproc()
  * allocate 2 pages. one for the user stack, the other one for guard page.
  * push the arg and set up esp, eip in trapframe.
  * sharing the same pgdir with other threads and main_thread.
  * the pid of thread is same with main_thread, but has unique indentifier "tid"
  *
  * @param[out]   threads         save the tid allocated by main_thread.
  * @param[in]    start_routine   the address of start funcion
  * @param[in]    arg             passed to the start function
  * @return       On success, return 0. On error, return non-zero value.
*/
int thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg) {
  int i;
  uint sz, sp, ustack[3], reuse;
  struct proc *nt, *p;
  struct proc *curproc = myproc();

  if((nt = allocproc()) == 0) {
    cprintf("cannot allocate new thread\n");
    return -1;
  }

  // make stack
  sz = curproc->sz;
  sz = PGROUNDUP(sz);

  reuse = 0;
  // if there is usable stack pages, use that pages.
  for(i = 0;i < NPTHD; i++) {
    if(curproc->main_thd->usable_stack[i] != 0) {
      reuse = curproc->main_thd->usable_stack[i];
      curproc->main_thd->usable_stack[i] = 0;
      break;
    }
  }

  // allocate two pages, one for guard page, the other one for stack
  if(reuse != 0) { // reuse the usable_stack
    if((reuse = allocuvm(curproc->pgdir, reuse, reuse + 2*PGSIZE)) == 0) {
      cprintf("cannot allocate new stack in thread_create\n");
      return -1;
    }
    clearpteu(curproc->pgdir, (char*)(reuse - 2*PGSIZE));
    sp = reuse;
  }
  else { // there is no usable_stack
    if((sz = allocuvm(curproc->pgdir, sz, sz + 2*PGSIZE)) == 0) {
      cprintf("cannot allocate new stack in thread_create\n");
      return -1;
    }
    clearpteu(curproc->pgdir, (char*)(sz - 2*PGSIZE));
    sp = sz;
  }

  // fill arg
  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = (int)arg;
  ustack[2] = 0;

  sp -= 12;

  if(copyout(curproc->pgdir, sp, ustack, 12) < 0) {
    cprintf("cannot copy ustack in thread_create\n");
    return -1;
  }

  acquire(&ptable.lock);
  *nt->tf = *curproc->tf;
  curproc->sz = sz;
  nt->sz = sz;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->pid == curproc->pid) {
      p->sz = sz;
    }
  }

  nt->pgdir = curproc->pgdir; // sharing pgdir
  nt->tf->eip = (int)start_routine; // set eip
  nt->tf->esp = sp; // set user stack.
  nt->pid = curproc->pid;
  nt->parent = curproc->parent;

  nt->tid = curproc->main_thd->nexttid++;
  nt->main_thd = curproc->main_thd;
  curproc->main_thd->thd_num++;
  *thread = nt->tid;


  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      nt->ofile[i] = filedup(curproc->ofile[i]);

  nt->cwd = idup(curproc->cwd);

  safestrcpy(nt->name, curproc->name, sizeof(curproc->name));

  nt->state = RUNNABLE;
  split_portion(curproc);

  release(&ptable.lock);

  return 0;
}

/**
  * This function is used to exit thread.
  * close all files, set up the return value.
  * wake up the main_thread.
  * set state to ZOMBIE

  * @parma[in] return value should be passed to main thread.
*/
void thread_exit(void *retval) {
  struct proc *curproc = myproc();
  int fd;

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;
  acquire(&ptable.lock);

  wakeup1(curproc->main_thd);

  curproc->main_thd->retvalue_array[(curproc->tid)%NPTHD] = retval;

  curproc->state = ZOMBIE;
  split_portion(curproc);
  // curproc->main_thd->thd_num--;
  sched();
}

/**
  * This function is used to wait the worker threads and clean up the resources.
  * If there is ZOMBIE thread, clean up user stack and kstack, unless wait.
  * @param[in]    thread    target tid.
  * @param[out]   retval    save the retval from thread_exit.
  * @return       On success, return zero. On error return non-zero value.
*/
int thread_join(thread_t thread, void **retval) {
  struct proc *p;
  struct proc *curproc = myproc();
  uint sp, a, pa;
  pte_t *pte;
  int i;

  if(retval == 0) {
    cprintf("ERROR: retval is null pointer\n");
    return -1;
  }
  // if(curproc->killed == 1) exit();

  acquire(&ptable.lock);

  while(1) {

    for(p = ptable.proc; p < & ptable.proc[NPROC]; p++) {
      if(p->pid == curproc->pid && p->tid == thread) { // if p is the thread
        if(p->state == RUNNABLE) { // the thread is still running
          sleep(curproc, &ptable.lock);
        }
        if(p->state == ZOMBIE) {
          // clean up this thread
          *retval = curproc->main_thd->retvalue_array[thread%NPTHD];
          // deallocate the guard, stack pages.
          sp = p->tf->esp;
          a = PGROUNDDOWN(sp);
          a -= PGSIZE;
          // save the stack page to main_thread
          for(i = 0; curproc->usable_stack[i] != 0 && i < NPTHD; i++);
          curproc->usable_stack[i] = a;
          // deallocate the stack like deallocuvm()
          for(; a  <= PGROUNDDOWN(sp); a += PGSIZE){ //only dealloc guard, stack
            pte = walkpgdir(p->pgdir, (char*)a, 0);
            if(!pte)
              a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
            else if((*pte & PTE_P) != 0){
              pa = PTE_ADDR(*pte);
              if(pa == 0)
                panic("kfree");
              char *v = P2V(pa);
              kfree(v);
              *pte = 0;
            }
          }

          kfree(p->kstack);
          p->kstack = 0;
          p->name[0] = 0;
          p->state = UNUSED;
          curproc->main_thd->thd_num--;
          split_portion(curproc);
          // procdump();
          release(&ptable.lock);

          return 0;
        }
      }
    }
  }
  cprintf("ERROR: in thread_join");
  release(&ptable.lock);

  return -1;
}

// wrapper function for thrad_create
int sys_thread_create(void) {
  int thread, start_routine, arg;
  if(argint(0, &thread) < 0 || argint(1, &start_routine) < 0 ||
    argint(2, &arg) < 0) {
    return -1;
  }

  return thread_create((thread_t *)thread, (void *)start_routine, (void *)arg);
}

// wrapper function for thread_join
int sys_thread_join(void) {
  int thread, retval;
  if(argint(0, &thread) < 0 || argint(1, &retval) < 0) {
    return -1;
  }

  return thread_join(thread, (void **)retval);
}

// wrapper function for thread_exit
int sys_thread_exit(void) {
  int retval;
  if(argint(0, &retval) < 0) {
    return -1;
  }

  thread_exit((void *)retval);
  return 0;
}

/**
  * This function is used to split CPU sharing portion
  * should acquire the ptable.lock before calling thsi function
  * @param[in]      main    should split the portion of main process.
  * @return         On success, return zero.
*/
int split_portion(struct proc *main) {

  struct proc *p;
  int cnt = 0;
  int split, rest;
  int total_portion = 0;

  if(main->isShare == 0) {
    return 0;
  }

  // count the all sharing portion and the number of threads.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->pid == main->pid) {
      total_portion += p->portion;
      if(p->state == RUNNING || p->state == RUNNABLE/*|| p->state == SLEEPING*/) {
        cnt++;
      }
    }
  }
  if(cnt != 0) {
    split = total_portion / cnt;
    rest = total_portion % cnt;
  }

  // set portion about running thread.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->pid == main->pid) {
      p->isShare = 1;
      if(p->state == RUNNING || p->state == RUNNABLE/*|| p->state == SLEEPING*/) {
        if(rest > 0) {
          p->portion = split + 1;
          rest--;
        }
        else p->portion = split;

        p->pass = main->pass;
        if(p->portion != 0) {
          p->stride = STRIDE_LARGE_NUM / p->portion;
        }
      }
    }
  }

  return 0;
}

/**
  * This function is used to exit reamaining threads.
  * It is only called by exit().
  * The state of main thread should be ZOMBIE.
  * And all threads except one thread which calls this function should be UNUSED
  * close all file, and clean up user stack, kstack.
  * @param[in]    thd     should be exited.
  * @returns      On success, return zero.
*/
int exit_all_thread1(struct proc* thd) {
  struct proc *p;
  int fd;
  uint sp, a, pa;
  pte_t *pte;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->pid == thd->pid && p != thd && (p->state == RUNNABLE || p->state == SLEEPING)) { // not calling p,
      // p is matching thread
      for(fd = 0; fd < NOFILE; fd++) {
        if(p->ofile[fd]) {
          fileclose(p->ofile[fd]);
          p->ofile[fd] = 0;
        }
      }
      begin_op();
      iput(p->cwd);
      end_op();
      p->cwd = 0;
    }
  }

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    // deallocate the guard, stack pages.
    if(p->pid == thd->pid && p->tid != 0 && p != thd && (p->state == RUNNABLE || p->state == SLEEPING)) {
      sp = p->tf->esp;
      a = PGROUNDDOWN(sp);
      a -= PGSIZE;

      // deallocate the stack like deallocuvm()
      for(; a  <= PGROUNDDOWN(sp); a += PGSIZE){ //only dealloc guard, stack
        pte = walkpgdir(p->pgdir, (char*)a, 0);
        if(!pte)
          a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
        else if((*pte & PTE_P) != 0){
          pa = PTE_ADDR(*pte);
          if(pa == 0)
            panic("kfree");
          char *v = P2V(pa);
          kfree(v);
          *pte = 0;
        }
      }

      kfree(p->kstack);
      p->kstack = 0;
      p->name[0] = 0;
      p->state = UNUSED;
    }

    if(p->pid == thd->pid && p->tid == 0) { // main thread should be ZOMBIE
      p->state = ZOMBIE;
    }
  } // end ptable for-loop
  return 0;
}

/**
  * This function is used to clean all remaining threads.
  * It is only called by wait().
  * If the parent wakes up, the child thread should be cleaned up.
  * @param[in]      main       all threads which have same pid with main
                                should be cleaned
  * @return On success, return zero.
*/
int clean_all_thread(struct proc *main) {
  struct proc *p;
  uint sp, a, pa;
  pte_t *pte;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(main->pid == p->pid && p->tid != 0) {
      if(p->state == ZOMBIE) {
        // clean up this thread
        // deallocate the guard, stack pages.
        sp = p->tf->esp;
        a = PGROUNDDOWN(sp);
        a -= PGSIZE;
        // save the stack page to main_thread
        // deallocate the stack like deallocuvm()
        for(; a  <= PGROUNDDOWN(sp); a += PGSIZE){ //only dealloc guard, stack
          pte = walkpgdir(p->pgdir, (char*)a, 0);
          if(!pte)
            a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
          else if((*pte & PTE_P) != 0){
            pa = PTE_ADDR(*pte);
            if(pa == 0)
              panic("kfree");
            char *v = P2V(pa);
            kfree(v);
            *pte = 0;
          }
        }

        kfree(p->kstack);
        p->kstack = 0;
        p->name[0] = 0;
        p->state = UNUSED;

      }
    }
  }
  return 0;
}

/**
  * This function is used to exit all remaining threads.
  * It is only called by exec()
  * @param[in]    cp      exit all threads which have same pid wich cp.
  * @return       On success, return zero.
*/
int exit_all_thread2(struct proc *cp) {
  struct proc *p;
  int fd;
  uint sp, a, pa;
  pte_t *pte;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->pid == cp->pid && p != cp && (p->state == RUNNABLE || p->state == SLEEPING)) { // not calling p,
      // p is matching thread
      for(fd = 0; fd < NOFILE; fd++) {
        if(p->ofile[fd]) {
          fileclose(p->ofile[fd]);
          p->ofile[fd] = 0;
        }
      }
      begin_op();
      iput(p->cwd);
      end_op();
      p->cwd = 0;
    }
  }

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    // deallocate the guard, stack pages.
    if(p->pid == cp->pid && p->tid != 0 && p != cp && (p->state == RUNNABLE || p->state == SLEEPING)) {
      sp = p->tf->esp;
      a = PGROUNDDOWN(sp);
      a -= PGSIZE;

      // deallocate the stack like deallocuvm()
      for(; a  <= PGROUNDDOWN(sp); a += PGSIZE){ //only dealloc guard, stack
        pte = walkpgdir(p->pgdir, (char*)a, 0);
        if(!pte)
          a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
        else if((*pte & PTE_P) != 0){
          pa = PTE_ADDR(*pte);
          if(pa == 0)
            panic("kfree");
          char *v = P2V(pa);
          kfree(v);
          *pte = 0;
        }
      }

      kfree(p->kstack);
      p->kstack = 0;
      p->name[0] = 0;
      p->state = UNUSED;
    }

    if(p->pid == cp->pid && p->tid == 0) {
      p->state = UNUSED;
    }
  } // end ptable for-loop
  return 0;
}

// turn on the killed bit of matching thread.
int kill_all_thread(int pid) {
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->pid == pid) {
      p->killed = 1;
    }
  }
  return 0;
}

// prepare the virtual address of stack pages, for threads.
int prepare_stack(struct proc* pick) {
  uint sz;
  int i;

  sz = pick->sz;
  sz += 8 * PGSIZE;
  for(i = 0; i < NPTHD; i++) {
    pick->usable_stack[i] = sz + PGSIZE * 2 * i;
  }
  sz += (2 * NPTHD + 1) * PGSIZE;

  clearpteu(pick->pgdir, (char*)(sz - PGSIZE));

  pick->isSet = 1;
  pick->sz = sz;
  return 0;
}
