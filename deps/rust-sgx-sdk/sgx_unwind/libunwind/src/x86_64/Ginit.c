/* libunwind - a platform-independent unwind library
   Copyright (C) 2002 Hewlett-Packard Co
   Copyright (C) 2007 David Mosberger-Tang
        Contributed by David Mosberger-Tang <dmosberger@gmail.com>

   Modified for x86_64 by Max Asbock <masbock@us.ibm.com>

This file is part of libunwind.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "unwind_i.h"

#ifdef UNW_REMOTE_ONLY

/* unw_local_addr_space is a NULL pointer in this case.  */
unw_addr_space_t unw_local_addr_space;

#else /* !UNW_REMOTE_ONLY */

static struct unw_addr_space local_addr_space;

unw_addr_space_t unw_local_addr_space = &local_addr_space;

static void
put_unwind_info (unw_addr_space_t as, unw_proc_info_t *proc_info, void *arg)
{
  /* it's a no-op */
}

static int
get_dyn_info_list_addr (unw_addr_space_t as, unw_word_t *dyn_info_list_addr,
                        void *arg)
{
#ifndef UNW_LOCAL_ONLY
# pragma weak _U_dyn_info_list_addr
  if (!_U_dyn_info_list_addr)
    return -UNW_ENOINFO;
#endif
  // Access the `_U_dyn_info_list` from `LOCAL_ONLY` library, i.e. libunwind.so.
  *dyn_info_list_addr = _U_dyn_info_list_addr ();
  return 0;
}

#define PAGE_SIZE 4096
#define PAGE_START(a)   ((a) & ~(PAGE_SIZE-1))

static int mem_validate_pipe[2] = {-1, -1};

static inline void
open_pipe (void)
{
  #ifndef HAVE_SGX
  /* ignore errors for closing invalid fd's */
  close (mem_validate_pipe[0]);
  close (mem_validate_pipe[1]);

  pipe2 (mem_validate_pipe, O_CLOEXEC | O_NONBLOCK);
  #endif
}

ALWAYS_INLINE
static int
write_validate (void *addr)
{
  #ifndef HAVE_SGX
  int ret = -1;
  ssize_t bytes = 0;

  do
    {
      char buf;
      bytes = read (mem_validate_pipe[0], &buf, 1);
    }
  while ( errno == EINTR );

  int valid_read = (bytes > 0 || errno == EAGAIN || errno == EWOULDBLOCK);
  if (!valid_read)
    {
      // re-open closed pipe
      open_pipe ();
    }

  do
    {
      /* use syscall insteadof write() so that ASAN does not complain */
      ret = syscall (SYS_write, mem_validate_pipe[1], addr, 1);
    }
  while ( errno == EINTR );

  return ret;
#else
  /*
   * This function's purpose:
   * https://github.com/libunwind/libunwind/commit/836c91c43d7a996028aa7e8d1f53630a6b8e7cbe#diff-98fa8f303410f32876910fb84c203cd6
   *
   * The calls to mincore() or msync() are not checking for actual accessibility this could lead to SIGSEGV if the address from a mapped page with the
   * PROT_NONE property occurs on the stack. Hence an attempt to write one byte from the checked address to a pipe will fail if the address is not readable.
   *
   *
   * But we think this function isn't needed inside SGX Enclave.
  */
  return 0;
#endif
}

static int (*mem_validate_func) (void *addr, size_t len);
static int msync_validate (void *addr, size_t len)
{
#ifdef HAVE_SGX
  extern int sgx_is_within_enclave(const void *addr, size_t size);
  if(!sgx_is_within_enclave(addr, len))
    return -1;
#endif

  if (msync (addr, len, MS_ASYNC) != 0)
    {
      return -1;
    }

  return write_validate (addr);
}

#ifdef HAVE_MINCORE
static int mincore_validate (void *addr, size_t len)
{
  unsigned char mvec[2]; /* Unaligned access may cross page boundary */
  size_t i;

  /* mincore could fail with EAGAIN but we conservatively return -1
     instead of looping. */
  if (mincore (addr, len, mvec) != 0)
    {
      return -1;
    }

  for (i = 0; i < (len + PAGE_SIZE - 1) / PAGE_SIZE; i++)
    {
      if (!(mvec[i] & 1)) return -1;
    }
  
  return write_validate (addr);
}
#endif

/* Initialise memory validation method. On linux kernels <2.6.21,
   mincore() returns incorrect value for MAP_PRIVATE mappings,
   such as stacks. If mincore() was available at compile time,
   check if we can actually use it. If not, use msync() instead. */
HIDDEN void
tdep_init_mem_validate (void)
{
  open_pipe ();

#ifdef HAVE_MINCORE
  unsigned char present = 1;
  unw_word_t addr = PAGE_START((unw_word_t)&present);
  unsigned char mvec[1];
  int ret;
  while ((ret = mincore ((void*)addr, PAGE_SIZE, mvec)) == -1 &&
         errno == EAGAIN) {}
  if (ret == 0 && (mvec[0] & 1))
    {
      Debug(1, "using mincore to validate memory\n");
      mem_validate_func = mincore_validate;
    }
  else
#endif
    {
      Debug(1, "using msync to validate memory\n");
      mem_validate_func = msync_validate;
    }
}

/* Cache of already validated addresses */
#define NLGA 4
static unw_word_t last_good_addr[NLGA];
static int lga_victim;

static int
validate_mem (unw_word_t addr)
{
  int i, victim;
  size_t len;

  if (PAGE_START(addr + sizeof (unw_word_t) - 1) == PAGE_START(addr))
    len = PAGE_SIZE;
  else
    len = PAGE_SIZE * 2;

  addr = PAGE_START(addr);

  if (addr == 0)
    return -1;

  for (i = 0; i < NLGA; i++)
    {
      if (last_good_addr[i] && (addr == last_good_addr[i]))
        return 0;
    }

  if (mem_validate_func ((void *) addr, len) == -1)
    return -1;

  victim = lga_victim;
  for (i = 0; i < NLGA; i++) {
    if (!last_good_addr[victim]) {
      last_good_addr[victim++] = addr;
      return 0;
    }
    victim = (victim + 1) % NLGA;
  }

  /* All slots full. Evict the victim. */
  last_good_addr[victim] = addr;
  victim = (victim + 1) % NLGA;
  lga_victim = victim;

  return 0;
}

static int
access_mem (unw_addr_space_t as, unw_word_t addr, unw_word_t *val, int write,
            void *arg)
{
  if (unlikely (write))
    {
      Debug (16, "mem[%016lx] <- %lx\n", addr, *val);
      *(unw_word_t *) addr = *val;
    }
  else
    {
      /* validate address */
      const struct cursor *c = (const struct cursor *)arg;
      if (likely (c != NULL) && unlikely (c->validate)
          && unlikely (validate_mem (addr))) {
        Debug (16, "mem[%016lx] -> invalid\n", addr);
        return -1;
      }
      *val = *(unw_word_t *) addr;
      Debug (16, "mem[%016lx] -> %lx\n", addr, *val);
    }
  return 0;
}

static int
access_reg (unw_addr_space_t as, unw_regnum_t reg, unw_word_t *val, int write,
            void *arg)
{
  unw_word_t *addr;
  ucontext_t *uc = ((struct cursor *)arg)->uc;

  if (unw_is_fpreg (reg))
    goto badreg;

  if (!(addr = x86_64_r_uc_addr (uc, reg)))
    goto badreg;

  if (write)
    {
      *(unw_word_t *) addr = *val;
      Debug (12, "%s <- 0x%016lx\n", unw_regname (reg), *val);
    }
  else
    {
      *val = *(unw_word_t *) addr;
      Debug (12, "%s -> 0x%016lx\n", unw_regname (reg), *val);
    }
  return 0;

 badreg:
  Debug (1, "bad register number %u\n", reg);
  return -UNW_EBADREG;
}

static int
access_fpreg (unw_addr_space_t as, unw_regnum_t reg, unw_fpreg_t *val,
              int write, void *arg)
{
  ucontext_t *uc = ((struct cursor *)arg)->uc;
  unw_fpreg_t *addr;

  if (!unw_is_fpreg (reg))
    goto badreg;

  if (!(addr = x86_64_r_uc_addr (uc, reg)))
    goto badreg;

  if (write)
    {
      Debug (12, "%s <- %08lx.%08lx.%08lx\n", unw_regname (reg),
             ((long *)val)[0], ((long *)val)[1], ((long *)val)[2]);
      *(unw_fpreg_t *) addr = *val;
    }
  else
    {
      *val = *(unw_fpreg_t *) addr;
      Debug (12, "%s -> %08lx.%08lx.%08lx\n", unw_regname (reg),
             ((long *)val)[0], ((long *)val)[1], ((long *)val)[2]);
    }
  return 0;

 badreg:
  Debug (1, "bad register number %u\n", reg);
  /* attempt to access a non-preserved register */
  return -UNW_EBADREG;
}

static int
get_static_proc_name (unw_addr_space_t as, unw_word_t ip,
                      char *buf, size_t buf_len, unw_word_t *offp,
                      void *arg)
{
#ifdef HAVE_SGX
  return -1;
#else
  return _Uelf64_get_proc_name (as, getpid (), ip, buf, buf_len, offp);
#endif
}

HIDDEN void
x86_64_local_addr_space_init (void)
{
  memset (&local_addr_space, 0, sizeof (local_addr_space));
  local_addr_space.caching_policy = UNWI_DEFAULT_CACHING_POLICY;
  local_addr_space.acc.find_proc_info = dwarf_find_proc_info;
  local_addr_space.acc.put_unwind_info = put_unwind_info;
  local_addr_space.acc.get_dyn_info_list_addr = get_dyn_info_list_addr;
  local_addr_space.acc.access_mem = access_mem;
  local_addr_space.acc.access_reg = access_reg;
  local_addr_space.acc.access_fpreg = access_fpreg;
  local_addr_space.acc.resume = x86_64_local_resume;
  local_addr_space.acc.get_proc_name = get_static_proc_name;
  unw_flush_cache (&local_addr_space, 0, 0);

  memset (last_good_addr, 0, sizeof (unw_word_t) * NLGA);
  lga_victim = 0;
}

#endif /* !UNW_REMOTE_ONLY */
