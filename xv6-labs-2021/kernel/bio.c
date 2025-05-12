// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

extern uint ticks;


struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // struct buf head;

} bcache;

struct hashtable{
  struct spinlock lock;
  struct buf head;
} table[NBUCKET];

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  for(b = bcache.buf; b < bcache.buf+NBUF; b++) {
    initsleeplock(&b->lock, "buffer");
    b->blockno = -1;
  }

  for(int i = 0; i < NBUCKET; i++){
    char lockname[8];
    snprintf(lockname, sizeof(lockname), "bcache_%d", i);
    initlock(&table[i].lock, lockname);
    table[i].head.next = &table[i].head;
  }
}

int
hash(int blockno)
{
  return blockno % NBUCKET;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // Is the block already cached?
  int idx = hash(blockno);
  struct hashtable *bucket = &table[idx];
  acquire(&bucket->lock);

  for(b = bucket->head.next; b != &bucket->head; b = b->next) {
    if(b->dev == dev && b->blockno == blockno) {
      ++b->refcnt;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  uint mintime = 0xffffffff;
  struct buf *replbuf = 0;
  for(b = bucket->head.next; b != &bucket->head; b = b->next) {
    if(b->refcnt == 0 && b->timestamp < mintime) {
      mintime = b->timestamp;
      replbuf = b;
    }
  }
  if(replbuf)
    goto find;

  acquire(&bcache.lock);
  struct hashtable *rbucket;

  for(int i = 0; ; i++) {
    mintime = 0xffffffff;
    for(b = bcache.buf; b < bcache.buf + NBUF; b++) {
      if(b->refcnt == 0 && b->timestamp < mintime) {
        mintime = b->timestamp;
        replbuf = b;
      }
    }
    if(replbuf) {
      if(replbuf->blockno == -1) {
        replbuf->next = bucket->head.next;
        bucket->head.next = replbuf;
        release(&bcache.lock);
        goto find;
      }
      int ridx = hash(replbuf->blockno);
      rbucket = &table[ridx];
      acquire(&rbucket->lock);
      if(b->refcnt == 0)
        break;
      release(&rbucket->lock);
    }
    /* if (i == 10000)
      panic("bget: no buffers"); */
  }
    
  for(b = &rbucket->head; b->next != replbuf; b++) {}

  b->next = replbuf->next;
  release(&rbucket->lock);
  replbuf->next = bucket->head.next;
  bucket->head.next = replbuf;
  release(&bcache.lock);
  goto find;

  panic("bget: no buffers");
  find:
    replbuf->dev = dev;
    replbuf->blockno = blockno;
    replbuf->valid = 0;
    replbuf->refcnt = 1;
    release(&bucket->lock);
    acquiresleep(&replbuf->lock);
    return replbuf;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int idx = hash(b->blockno);
  acquire(&table[idx].lock);
  
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->timestamp = ticks;
  }
  
  release(&table[idx].lock);
}

void
bpin(struct buf *b) {
  int idx = hash(b->blockno);
  acquire(&table[idx].lock);
  b->refcnt++;
  release(&table[idx].lock);
}

void
bunpin(struct buf *b) {
  int idx = hash(b->blockno);
  acquire(&table[idx].lock);
  b->refcnt--;
  release(&table[idx].lock);
}



