/*
Project 3 by SR Yoon
@since 2018.05.24
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

// thread_create
int thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg)
{
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
  // if there is usable stack pages, use that pages
  for(i = 0;i < NPTHD; i++) {
    if(curproc->main_thd->usable_stack[i] != 0) {
      reuse = curproc->main_thd->usable_stack[i];
      curproc->main_thd->usable_stack[i] = 0;
      break;
    }
  }

  // allocate two pages: one for guard page, the other for stack
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
  nt->tf->esp = sp; // set user stack
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


// thread_exit
void thread_exit(void *retval)
{
  struct proc *curproc = myproc();
  int fd;

  // Close all open files
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


// thread_join
int thread_join(thread_t thread, void **retval)
{
  struct proc *p;
  struct proc *curproc = myproc();
  uint sp, a, pa;
  pte_t *pte;
  int i;

  if(retval == 0) {
    cprintf("ERROR: retval is null pointer\n");
    return -1;
  }

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
          // deallocate the guard, stack pages
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

// This function is used to split CPU sharing portion
int split_portion(struct proc *main)
{
  struct proc *p;
  int cnt = 0;
  int split, rest;
  int total_portion = 0;

  if(main->isShare == 0) {
    return 0;
  }

  // count the all sharing portion and the number of threads
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

  // set portion about running thread
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

// This function is used to exit remaining threads
int exit_all_thread1(struct proc* thd)
{
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

// This function is used to clean all remaining threads
int clean_all_thread(struct proc *main)
{
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

// This function is used to exit all ramaining threads
int exit_all_thread2(struct proc *cp)
{
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
    // deallocate the guard, stack pages
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

// Turn on the killed bit of matching thread
int kill_all_thread(int pid)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->pid == pid) {
      p->killed = 1;
    }
  }
  return 0;
}

// Prepare the virtual address of stack pages, for threads
int prepare_stack(struct proc* pick)
{
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
