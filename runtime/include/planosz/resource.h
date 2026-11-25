/* Runtime resource lock table (bit-masks; admission guarantees no conflicts, this enforces it). */
#ifndef PLANOSZ_RESOURCE_H_
#define PLANOSZ_RESOURCE_H_

#include <stdint.h>

void planosz_res_reset(void);
/* 0 on success, -1 (busy) if an exclusive claim would overlap. */
int planosz_res_acquire(uint32_t excl_mask, uint32_t shared_mask);
void planosz_res_release(uint32_t excl_mask, uint32_t shared_mask);

#endif /* PLANOSZ_RESOURCE_H_ */
