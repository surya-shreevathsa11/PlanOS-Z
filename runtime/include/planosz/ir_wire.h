/* IR wire format v1 (UART / shell upload) and the ephemeral upload slot.
 *
 *   'P' 'Z' | version u8 | flags u8 | perm_mask u32 | deadline_ms u32 | n_steps u8 | plan_id_len u8 | plan_id
 *   n_steps x { id_len u8 | id | skill u8 | deps_mask u32 | confirm u8 | when_step i8 | when_op u8 |
 *               when_value_milli i32 | n_args u8 | args i32 x n_args | host_wcet_ms u32 }
 *   crc16 u16  (CRC-16/CCITT-FALSE over all preceding bytes), all integers little-endian
 *
 * No resource masks travel on the wire: the device derives them. host_wcet_ms is advisory
 * (only used to report a mismatch). Any framing problem - bad magic, CRC, unknown version,
 * truncation, trailing bytes, oversize, unsafe identifier characters - is
 * PLANOSZ_REJECT_UNSUPPORTED_IR. Semantic problems are admission's job. Python mirror:
 * tools/planoszc/planoszc/wire.py. */
#ifndef PLANOSZ_IR_WIRE_H_
#define PLANOSZ_IR_WIRE_H_

#include <stddef.h>
#include <stdint.h>
#include "planosz/errors.h"
#include "planosz/ir.h"

#define PLANOSZ_PLAN_ID_MAX 31 /* chars */
#define PLANOSZ_STEP_ID_MAX 23
#define PLANOSZ_UPLOAD_MAX_BYTES 2048

/* Storage the decoded plan points into (ids are copied, nothing aliases the input buffer). */
struct planosz_ir_slot {
	struct planosz_ir_plan plan;
	struct planosz_ir_step steps[PLANOSZ_MAX_STEPS];
	char plan_id[PLANOSZ_PLAN_ID_MAX + 1];
	char ids[PLANOSZ_MAX_STEPS][PLANOSZ_STEP_ID_MAX + 1];
};

uint16_t planosz_crc16(const uint8_t *data, size_t len, uint16_t crc);
/* Standard base64 (padding allowed). 0 on success, -1 on bad input or overflow. */
int planosz_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap, size_t *out_len);
/* PLANOSZ_OK or PLANOSZ_REJECT_UNSUPPORTED_IR (*why = short static reason, may be NULL). */
enum planosz_code planosz_ir_decode(const uint8_t *buf, size_t len, struct planosz_ir_slot *slot,
				    const char **why);

/* Chunked upload: begin -> data* -> end. One slot; `begin` invalidates the previous plan. */
void planosz_upload_begin(void);
enum planosz_code planosz_upload_data(const char *b64, const char **why);
enum planosz_code planosz_upload_end(const char **why);
/* The uploaded plan, or NULL when the slot is empty / invalid. Name used by the shell: "uploaded". */
const struct planosz_ir_plan *planosz_upload_plan(void);
size_t planosz_upload_bytes(void);

#endif /* PLANOSZ_IR_WIRE_H_ */
