/* gl_jit.h -- test-rig placement of the translation caches (gl_jit.c). */
#ifndef GL_JIT_H
#define GL_JIT_H

void *map_jit_block(unsigned size);
void unmap_jit_block(void *bufptr, unsigned size);
/* 0 near, 1 far, 2 split (GL_JIT) */
int gl_jit_mode(void);
/* GL_JIT=split: point *ram_cache at a block of its own.  1 = moved, 0 = not
 * split mode, -1 = no memory. */
int gl_jit_split_ram(unsigned char **ram_cache, unsigned ram_size);

#endif
