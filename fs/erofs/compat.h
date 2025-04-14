#ifndef __EROFS_COMPAT_H
#define __EROFS_COMPAT_H

#include <linux/wait.h>

/**
 * clear_and_wake_up_bit - clear a bit and wake up anyone waiting on that bit
 *
 * @bit: the bit of the word being waited on
 * @word: the word being waited on, a kernel virtual address
 *
 * You can use this helper if bitflags are manipulated atomically rather than
 * non-atomically under a lock.
 */
static inline void clear_and_wake_up_bit(int bit, void *word)
{
	clear_bit_unlock(bit, word);
	/* See wake_up_bit() for which memory barrier you need to use. */
	smp_mb__after_atomic();
	wake_up_bit(word, bit);
}

#endif
