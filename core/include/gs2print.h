#ifndef GS2PRINT_H
#define GS2PRINT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gs2_job gs2_job;

gs2_job *gs2_job_new(void);
void gs2_job_free(gs2_job *job);

/* Power-on SW1-8. 0: a CR returns the head. 1: a CR also feeds one line.
 * Call after gs2_job_new and before the first write. ESC D / ESC Z still
 * override it for the rest of the job.
 */
void gs2_job_set_auto_lf(gs2_job *job, int on);

/* Consume this buffer and return. A command split across calls stays
 * in the decoder until a later write completes it. len may be 0.
 */
void gs2_job_write(gs2_job *job, const uint8_t *bytes, size_t len);

/* Commit the trailing page. An unfinished command is dropped. */
void gs2_job_finish(gs2_job *job);

int gs2_job_page_count(gs2_job *job);

/* Packed 1-bit, top-down, bit 7 is the leftmost pixel, 720 dpi.
 * NULL if page is out of range. The pointer is valid until the next
 * gs2_job_page_bits, gs2_job_write, or gs2_job_finish.
 */
const uint8_t *gs2_job_page_bits(gs2_job *job, int page,
                                 int *width, int *height, int *stride);

/* 0 on success, -1 on failure. */
int gs2_job_save_pdf(gs2_job *job, const char *path);

/* Offset of the printer bytes in a GS2PRINT 1 saved dump.
 * Returns 0 when the buffer is a raw dump.
 */
size_t gs2_dump_payload_offset(const uint8_t *bytes, size_t len);

/* Copies one GS2PRINT 1 header value into buf, NUL-terminated.
 * Returns 1 when the field is present, 0 when the buffer is not a
 * ticket or the field is absent, and -1 when the value does not fit.
 */
int gs2_ticket_field(const uint8_t *bytes, size_t len,
                     const char *key, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
