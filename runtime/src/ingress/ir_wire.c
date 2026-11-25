/* IR wire decoding, CRC, base64 and the ephemeral upload slot. Portable C (host-tested). */
#include <string.h>
#include "planosz/ir_wire.h"

uint16_t planosz_crc16(const uint8_t *data, size_t len, uint16_t crc)
{
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)((uint16_t)data[i] << 8);
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

static int b64_val(char c)
{
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	return c == '+' ? 62 : c == '/' ? 63 : -1;
}

int planosz_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
	size_t n = 0;

	if (len % 4 != 0) {
		return -1;
	}
	for (size_t i = 0; i < len; i += 4) {
		int v[4];
		int pad = 0;

		for (int k = 0; k < 4; k++) {
			char c = in[i + (size_t)k];

			if (c == '=') {
				if (i + 4 != len || k < 2) {
					return -1; /* padding only in the last quad, at position 2 or 3 */
				}
				v[k] = 0;
				pad++;
			} else if (pad || (v[k] = b64_val(c)) < 0) {
				return -1;
			}
		}
		uint32_t w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
		int bytes = 3 - pad;

		if (n + (size_t)bytes > cap) {
			return -1;
		}
		out[n++] = (uint8_t)(w >> 16);
		if (bytes > 1) {
			out[n++] = (uint8_t)(w >> 8);
		}
		if (bytes > 2) {
			out[n++] = (uint8_t)w;
		}
	}
	*out_len = n;
	return 0;
}

/* ---- decoder ---- */
struct rd {
	const uint8_t *p;
	size_t left;
	int bad;
};

static uint32_t rd_u(struct rd *r, unsigned int bytes)
{
	uint32_t v = 0;

	if (r->left < bytes) {
		r->bad = 1;
		r->left = 0;
		return 0;
	}
	for (unsigned int i = 0; i < bytes; i++) {
		v |= (uint32_t)r->p[i] << (8 * i);
	}
	r->p += bytes;
	r->left -= bytes;
	return v;
}

static int id_chars_ok(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char c = s[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
		      c == '.' || c == '-')) {
			return 0;
		}
	}
	return n > 0;
}

static enum planosz_code unsupported(const char **why, const char *msg)
{
	if (why) {
		*why = msg;
	}
	return PLANOSZ_REJECT_UNSUPPORTED_IR;
}

enum planosz_code planosz_ir_decode(const uint8_t *buf, size_t len, struct planosz_ir_slot *slot, const char **why)
{
	struct rd r;

	if (len < 16 || len > PLANOSZ_UPLOAD_MAX_BYTES) {
		return unsupported(why, "bad length");
	}
	if (buf[0] != 'P' || buf[1] != 'Z') {
		return unsupported(why, "bad magic");
	}
	if (planosz_crc16(buf, len - 2, 0xFFFFu) != (uint16_t)(buf[len - 2] | (buf[len - 1] << 8))) {
		return unsupported(why, "crc mismatch");
	}
	r.p = buf + 2;
	r.left = len - 4; /* header+body, without magic and crc */
	r.bad = 0;
	if (rd_u(&r, 1) != PLANOSZ_IR_VERSION) {
		return unsupported(why, "unsupported ir version");
	}
	if (rd_u(&r, 1) != 0) {
		return unsupported(why, "unsupported flags");
	}
	memset(slot, 0, sizeof(*slot));
	slot->plan.perm_mask = rd_u(&r, 4);
	slot->plan.deadline_ms = rd_u(&r, 4);
	const unsigned int n_steps = rd_u(&r, 1);
	const unsigned int idlen = rd_u(&r, 1);

	if (r.bad || n_steps == 0 || n_steps > PLANOSZ_MAX_STEPS || idlen == 0 || idlen > PLANOSZ_PLAN_ID_MAX ||
	    r.left < idlen) {
		return unsupported(why, "bad header");
	}
	memcpy(slot->plan_id, r.p, idlen);
	if (!id_chars_ok(slot->plan_id, idlen)) {
		return unsupported(why, "bad plan id");
	}
	r.p += idlen;
	r.left -= idlen;

	for (unsigned int i = 0; i < n_steps; i++) {
		struct planosz_ir_step *s = &slot->steps[i];
		const unsigned int sl = rd_u(&r, 1);

		if (r.bad || sl == 0 || sl > PLANOSZ_STEP_ID_MAX || r.left < sl) {
			return unsupported(why, "bad step id");
		}
		memcpy(slot->ids[i], r.p, sl);
		if (!id_chars_ok(slot->ids[i], sl)) {
			return unsupported(why, "bad step id");
		}
		r.p += sl;
		r.left -= sl;
		s->id = slot->ids[i];
		s->skill_idx = (uint8_t)rd_u(&r, 1);
		s->deps_mask = rd_u(&r, 4);
		const uint32_t confirm = rd_u(&r, 1);

		s->when_step = (int8_t)rd_u(&r, 1);
		s->when_op = (uint8_t)rd_u(&r, 1);
		s->when_value_milli = (int32_t)rd_u(&r, 4);
		const unsigned int n_args = rd_u(&r, 1);

		if (r.bad || confirm > 1 || n_args > PLANOSZ_MAX_ARGS) {
			return unsupported(why, "bad step record");
		}
		s->confirm = (uint8_t)confirm;
		for (unsigned int a = 0; a < n_args; a++) {
			s->args[a] = (int32_t)rd_u(&r, 4);
		}
		s->wcet_ms = rd_u(&r, 4); /* advisory host claim; never used for decisions */
		if (r.bad) {
			return unsupported(why, "truncated step");
		}
	}
	if (r.left != 0) {
		return unsupported(why, "trailing bytes");
	}
	slot->plan.plan_id = slot->plan_id;
	slot->plan.n_steps = (uint8_t)n_steps;
	slot->plan.steps = slot->steps;
	return PLANOSZ_OK;
}

/* ---- upload slot ---- */
static uint8_t upload_buf[PLANOSZ_UPLOAD_MAX_BYTES];
static size_t upload_len;
static int upload_err;
static const char *upload_err_why;
static struct planosz_ir_slot slot;
static int slot_valid;

void planosz_upload_begin(void)
{
	upload_len = 0;
	upload_err = 0;
	upload_err_why = NULL;
	slot_valid = 0;
}

enum planosz_code planosz_upload_data(const char *b64, const char **why)
{
	size_t n = 0;

	if (upload_err) {
		return unsupported(why, upload_err_why);
	}
	if (planosz_base64_decode(b64, strlen(b64), upload_buf + upload_len, sizeof(upload_buf) - upload_len, &n) != 0) {
		upload_err = 1;
		upload_err_why = "bad base64 or too large";
		return unsupported(why, upload_err_why);
	}
	upload_len += n;
	return PLANOSZ_OK;
}

enum planosz_code planosz_upload_end(const char **why)
{
	enum planosz_code rc;

	slot_valid = 0;
	if (upload_err) {
		return unsupported(why, upload_err_why);
	}
	rc = planosz_ir_decode(upload_buf, upload_len, &slot, why);
	slot_valid = rc == PLANOSZ_OK;
	return rc;
}

const struct planosz_ir_plan *planosz_upload_plan(void)
{
	return slot_valid ? &slot.plan : NULL;
}

size_t planosz_upload_bytes(void)
{
	return upload_len;
}
