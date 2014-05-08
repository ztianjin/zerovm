/*
 * Copyright (c) 2012, LiteStack, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <sys/mman.h>
#include "src/main/config.h"
#include "src/main/zlog.h"
#include "src/loader/userspace.h"
#include "src/loader/usermap.h"
#include "src/loader/context.h"
#include "src/channels/channel.h"
#include "src/channels/serializer.h"

/*
 * TODO(d'b): collect all defines in proper place under proper names
 */
#define ABSOLUTE_MMAP (MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE | MAP_FIXED)
#define TRAMP_IDX 2
#define TRAMP_PATTERN \
  0x48, 0xb8, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4, \
  0xf4, 0xf4, 0xff, 0xd0, 0xf4, 0xf4, 0xf4, 0xf4, \
  0xf4, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4, \
  0xf4, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4
#define PROXY_PTR ((void*)0x5AFECA110000)
#define PROXY_SIZE NACL_MAP_PAGESIZE
#define PROXY_IDX 2
#define PROXY_PATTERN \
  0x48, 0xb8, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4, 0xf4, 0xff, 0xe0
#define NULL_SIZE 0x10000
#define USER_PTR_SIZE sizeof(int32_t)
#define HALT_OPCODE 0xf4

/* should be kept in sync with struct UserManifest from api/zvm.h*/
struct UserManifest64
{
  uint32_t heap_ptr;
  uint32_t heap_size;
  uint32_t stack_size;
  int32_t channels_count;
  struct ChannelRec channels[0];
};

static uintptr_t heap_end = 0;
extern int SyscallSeg(); /* defined in to_trap.S */

uintptr_t UserHeapEnd()
{
  return NaClUserToSys(heap_end);
}

/* allocate and set call proxy */
static void MakeTrapProxy()
{
  int i;
  void *p;
  uint8_t pattern[] = { PROXY_PATTERN };

  /* allocate page for proxy */
  p = mmap(PROXY_PTR, PROXY_SIZE, PROT_WRITE, ABSOLUTE_MMAP, -1, 0);
  ZLOGFAIL(PROXY_PTR != p, errno, "cannot allocate proxy");

  /* populate proxy area with halts */
  memset(PROXY_PTR, HALT_OPCODE, PROXY_SIZE);

  /* patch pattern with trap address and update proxy */
  *(uintptr_t*)(pattern + PROXY_IDX) = (uintptr_t)&SyscallSeg;
  memcpy(PROXY_PTR, pattern, ARRAY_SIZE(pattern));

  /* change proxy protection */
  i = Zmprotect(PROXY_PTR, PROXY_SIZE, PROT_READ | PROT_EXEC | PROT_LOCK);
  ZLOGFAIL(0 != i, i, "cannot set proxy protection");
}

/* initialize trampoline using given thunk address */
static void SetTrampoline()
{
  int i;
  uint8_t pattern[] = { TRAMP_PATTERN };

  /* change protection of area to RW */
  i = Zmprotect((void*)(MEM_START + NACL_TRAMPOLINE_START),
      NACL_TRAMPOLINE_SIZE, PROT_READ | PROT_WRITE);
  ZLOGFAIL(0 != i, i, "cannot make trampoline writable");

  /* create trampoline call pattern */
  *(uintptr_t*)(pattern + TRAMP_IDX) = (uintptr_t)PROXY_PTR;

  /* populate trampoline area with it */
  for(i = 0; i < NACL_TRAMPOLINE_SIZE / ARRAY_SIZE(pattern); ++i)
    memcpy((void*)(MEM_START + NACL_TRAMPOLINE_START) + i * ARRAY_SIZE(pattern),
        pattern, ARRAY_SIZE(pattern));

  /* change protection of area to RXL */
  i = Zmprotect((void*)(MEM_START + NACL_TRAMPOLINE_START),
      NACL_TRAMPOLINE_SIZE, PROT_READ | PROT_EXEC | PROT_LOCK);
  ZLOGFAIL(0 != i, i, "cannot make trampoline executable");
}

/* free call proxy */
static void FreeTrapProxy()
{
  munmap(PROXY_PTR, PROXY_SIZE);
}

void MakeUserSpace()
{
  int i;
  void *p;

  /* get 84gb at the fixed address */
  p = mmap((void*)UNTRUSTED_START, UNTRUSTED_SIZE, PROT_NONE, ABSOLUTE_MMAP, -1, 0);
  ZLOGFAIL((void*)UNTRUSTED_START != p, errno, "cannot allocate 84gb");

  /* give advice to kernel */
  i = madvise((void*)UNTRUSTED_START, UNTRUSTED_SIZE, MADV_DONTNEED);
  ZLOGIF(i != 0, "cannot madvise 84gb: %s", strerror(errno));

  MakeTrapProxy();
}

void FreeUserSpace()
{
  munmap((void*)UNTRUSTED_START, 2 * GUARDSIZE + FOURGIG);
  FreeTrapProxy();
}

/* initialize stack */
static void SetStack()
{
  int i;

  /* change protection of stack to RWL */
  i = Zmprotect((void*)(MEM_START + FOURGIG - STACK_SIZE),
      STACK_SIZE, PROT_READ | PROT_WRITE | PROT_LOCK);
  ZLOGFAIL(0 != i, i, "cannot set stack protection");
}

/* set user manifest and initialize heap */
static void SetManifest(const struct Manifest *manifest)
{
  struct ChannelsSerial *channels;
  struct UserManifest64 *user;
  int64_t size;
  int i;

  assert(manifest != NULL);
  assert(manifest->channels != NULL);

  /* set user manifest to the user stack end */
  /*
   * TODO(d'b): current user manifest address is not perfect. stack size
   * is not intended to be constant. there are a few options:
   * 1. return user manifest through trap function
   * 2. find another fixed address. e.g. start of stack
   * note: fixed address could be set for user manifest address only. possible
   * solution can be placing this address into trampoline as part of nop
   * instruction, like "cmp eax, address". another profit of this solution:
   * this address cannot be changed by untrusted code (manifest can be changed
   * by uboot)
   */
  user = (void*)NaClUserToSys(FOURGIG - STACK_SIZE);

  /* serialize channels and copy to user manifest */
  channels = ChannelsSerializer(manifest, FOURGIG - STACK_SIZE + sizeof *user);
  memcpy(&user->channels, channels->channels, channels->size);

  /* set user manifest fields */
  user->heap_ptr = NACL_TRAMPOLINE_END;
  user->heap_size = heap_end - NACL_TRAMPOLINE_END;
  user->channels_count = manifest->channels->len;

  /* calculate and set stack size */
  size = ROUNDUP_64K(channels->size + sizeof *user);
  user->stack_size = STACK_SIZE - size;

  /* make the user manifest read only but not locked (for uboot) */
  /* TODO(d'b): avoid this hack */
  for(i = 0; i < size / NACL_MAP_PAGESIZE; ++i)
    GetUserMap()[i + (FOURGIG - STACK_SIZE) / NACL_MAP_PAGESIZE]
                 &= PROT_READ | PROT_WRITE;
  Zmprotect(user, size, PROT_READ);
}

/* calculate and set user heap */
static void SetHeap(const struct Manifest *manifest)
{
  uintptr_t i;
  int64_t heap;
  void *p;

  /* fail if memory size is invalid */
  ZLOGFAIL(manifest->mem_size <= 0 || manifest->mem_size > FOURGIG,
      ENOMEM, "invalid memory size");

  /* calculate user heap size (must be allocated next to the data_end) */
  p = (void*) ROUNDUP_64K(NACL_TRAMPOLINE_END);
  heap = manifest->mem_size - STACK_SIZE;
  heap = ROUNDUP_64K(heap) - ROUNDUP_64K(NACL_TRAMPOLINE_END);
  ZLOGFAIL(heap <= MIN_HEAP_SIZE, ENOMEM, "user heap size is too small");

  /* make heap RW */
  p = (void*) NaClUserToSys(NACL_TRAMPOLINE_END);
  i = Zmprotect(p, heap, PROT_READ | PROT_WRITE);
  ZLOGFAIL(0 != i, i, "cannot protection user heap");
  heap_end = NaClSysToUser((uintptr_t) p + heap);
}

void SetUserSpace(struct Manifest *manifest)
{
  SetTrampoline();
  SetStack();
  SetHeap(manifest);
  SetManifest(manifest);
  LockRestrictedMemory();
}
