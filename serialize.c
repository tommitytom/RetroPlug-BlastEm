#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "serialize.h"
#include "util.h"

#ifndef SERIALIZE_DEFAULT_SIZE
#define SERIALIZE_DEFAULT_SIZE (256*1024) //default to enough for a Genesis save state
#endif


void init_serialize(serialize_buffer *buf)
{
	buf->storage = SERIALIZE_DEFAULT_SIZE;
	buf->size = 0;
	buf->current_section_start = 0;
	buf->data = malloc(SERIALIZE_DEFAULT_SIZE);
	buf->failed = 0; /* RetroPlug: see the field in serialize.h */
}

/* RetroPlug: returns whether there is room to write, so every save_* can bail instead of writing through the NULL
 * the old form assigned straight over buf->data when a growth failed. A caller that wants a whole state checks
 * buf->failed, not the size. */
static uint8_t reserve(serialize_buffer *buf, size_t amount)
{
	if (buf->failed) {
		return 0;
	}
	if (amount > (buf->storage - buf->size)) {
		size_t storage = buf->storage;
		if (amount < storage) {
			storage *= 2;
		} else {
			//doublign isn't enough, increase by the precise amount needed
			storage += amount - (storage - buf->size);
		}
		uint8_t *data = realloc(buf->data, storage + sizeof(*buf));
		if (!data) {
			buf->failed = 1;
			return 0;
		}
		buf->data = data;
		buf->storage = storage;
	}
	return 1;
}

void save_int32(serialize_buffer *buf, uint32_t val)
{
	if (!reserve(buf, sizeof(val))) {
		return;
	}
	buf->data[buf->size++] = val >> 24;
	buf->data[buf->size++] = val >> 16;
	buf->data[buf->size++] = val >> 8;
	buf->data[buf->size++] = val;
}

void save_int16(serialize_buffer *buf, uint16_t val)
{
	if (!reserve(buf, sizeof(val))) {
		return;
	}
	buf->data[buf->size++] = val >> 8;
	buf->data[buf->size++] = val;
}

void save_int8(serialize_buffer *buf, uint8_t val)
{
	if (!reserve(buf, sizeof(val))) {
		return;
	}
	buf->data[buf->size++] = val;
}

void save_string(serialize_buffer *buf, char *val)
{
	size_t len = strlen(val);
	save_buffer8(buf, val, len);
}

void save_buffer8(serialize_buffer *buf, void *val, size_t len)
{
	if (!reserve(buf, len)) {
		return;
	}
	memcpy(&buf->data[buf->size], val, len);
	buf->size += len;
}

void save_buffer16(serialize_buffer *buf, uint16_t *val, size_t len)
{
	if (!reserve(buf, len * sizeof(*val))) {
		return;
	}
	for(; len != 0; len--, val++) {
		buf->data[buf->size++] = *val >> 8;
		buf->data[buf->size++] = *val;
	}
}

void save_buffer32(serialize_buffer *buf, uint32_t *val, size_t len)
{
	if (!reserve(buf, len * sizeof(*val))) {
		return;
	}
	for(; len != 0; len--, val++) {
		buf->data[buf->size++] = *val >> 24;
		buf->data[buf->size++] = *val >> 16;
		buf->data[buf->size++] = *val >> 8;
		buf->data[buf->size++] = *val;
	}
}

void start_section(serialize_buffer *buf, uint16_t section_id)
{
	save_int16(buf, section_id);
	//reserve some space for size once we end this section
	if (!reserve(buf, sizeof(uint32_t))) {
		return;
	}
	buf->size += sizeof(uint32_t);
	//save start point for use in end_device
	buf->current_section_start = buf->size;
}

void end_section(serialize_buffer *buf)
{
	if (buf->failed) {
		/* RetroPlug: this patches space start_section reserved; once the capture is refused there is no state here
		 * that anyone will read, and nothing to patch. */
		return;
	}
	size_t section_size = buf->size - buf->current_section_start;
	if (section_size > 0xFFFFFFFFU) {
		/* RetroPlug: was fatal_error. A capture that ran away is a refused capture, not a dead process. */
		buf->failed = 1;
		return;
	}
	uint32_t size = section_size;
	uint8_t *field = buf->data + buf->current_section_start - sizeof(uint32_t);
	*(field++) = size >> 24;
	*(field++) = size >> 16;
	*(field++) = size >> 8;
	*(field++) = size;
	buf->current_section_start = 0;
}

void register_section_handler(deserialize_buffer *buf, section_handler handler, uint16_t section_id)
{
	if (section_id > buf->max_handler) {
		uint16_t old_max = buf->max_handler;
		if (buf->max_handler < 0x8000) {
			buf->max_handler *= 2;
		} else {
			buf->max_handler = 0xFFFF;
		}
		buf->handlers = realloc(buf->handlers, (buf->max_handler+1) * sizeof(handler));
		memset(buf->handlers + old_max + 1, 0, (buf->max_handler - old_max) * sizeof(handler));
	}
	if (!buf->handlers) {
		buf->handlers = calloc(buf->max_handler + 1, sizeof(handler));
	}
	buf->handlers[section_id] = handler;
}

void init_deserialize(deserialize_buffer *buf, uint8_t *data, size_t size)
{
	buf->size = size;
	buf->cur_pos = 0;
	buf->data = data;
	buf->handlers = NULL;
	buf->max_handler = 8;
	buf->failed = 0; /* RetroPlug: see the field in serialize.h */
}

/* RetroPlug: every refusal below latches the buffer and leaves cur_pos at the end of it, so the caller's section loop
 * stops instead of reading the same short field forever, and the value a caller gets back is zero, which nothing
 * consumes before it has checked the latch. A state that cannot be read whole is a refusal the caller returns, not a
 * process that exits. */
uint32_t load_int32(deserialize_buffer *buf)
{
	uint32_t val = 0;
	if ((buf->size - buf->cur_pos) < sizeof(val)) {
		warning("Failed to load required int32 field\n");
		buf->failed = 1;
		buf->cur_pos = buf->size;
		return val;
	}
	val = buf->data[buf->cur_pos++] << 24;
	val |= buf->data[buf->cur_pos++] << 16;
	val |= buf->data[buf->cur_pos++] << 8;
	val |= buf->data[buf->cur_pos++];
	return val;
}

uint16_t load_int16(deserialize_buffer *buf)
{
	uint16_t val = 0;
	if ((buf->size - buf->cur_pos) < sizeof(val)) {
		warning("Failed to load required int16 field\n");
		buf->failed = 1;
		buf->cur_pos = buf->size;
		return val;
	}
	val = buf->data[buf->cur_pos++] << 8;
	val |= buf->data[buf->cur_pos++];
	return val;
}

uint8_t load_int8(deserialize_buffer *buf)
{
	uint8_t val = 0;
	if ((buf->size - buf->cur_pos) < sizeof(val)) {
		warning("Failed to load required int8 field\n");
		buf->failed = 1;
		buf->cur_pos = buf->size;
		return val;
	}
	val = buf->data[buf->cur_pos++];
	return val;
}

void load_buffer8(deserialize_buffer *buf, void *dst, size_t len)
{
	if ((buf->size - buf->cur_pos) < len) {
		warning("Failed to load required buffer of size %d\n", (int)len);
		buf->failed = 1;
		buf->cur_pos = buf->size;
		return;
	}
	memcpy(dst, buf->data + buf->cur_pos, len);
	buf->cur_pos += len;
}

void load_buffer16(deserialize_buffer *buf, uint16_t *dst, size_t len)
{
	if ((buf->size - buf->cur_pos) < len * sizeof(uint16_t)) {
		warning("Failed to load required buffer of size %d\n", (int)len);
		buf->failed = 1;
		buf->cur_pos = buf->size;
		return;
	}
	for(; len != 0; len--, dst++) {
		uint16_t value = buf->data[buf->cur_pos++] << 8;
		value |= buf->data[buf->cur_pos++];
		*dst = value;
	}
}
void load_buffer32(deserialize_buffer *buf, uint32_t *dst, size_t len)
{
	if ((buf->size - buf->cur_pos) < len * sizeof(uint32_t)) {
		warning("Failed to load required buffer of size %d\n", (int)len);
		buf->failed = 1;
		buf->cur_pos = buf->size;
		return;
	}
	for(; len != 0; len--, dst++) {
		uint32_t value = buf->data[buf->cur_pos++] << 24;
		value |= buf->data[buf->cur_pos++] << 16;
		value |= buf->data[buf->cur_pos++] << 8;
		value |= buf->data[buf->cur_pos++];
		*dst = value;
	}
}

void load_section(deserialize_buffer *buf)
{
	if (!buf->handlers) {
		/* RetroPlug: was fatal_error. A state cannot be walked without handlers, which is a refusal of that state. */
		warning("load_section called on a deserialize_buffer with no handlers registered\n");
		buf->failed = 1;
		buf->cur_pos = buf->size;
		return;
	}
	uint16_t section_id = load_int16(buf);
	uint32_t size = load_int32(buf);
	if (buf->failed) {
		/* RetroPlug: one of the two loads above refused, so there is no section here to read. */
		return;
	}
	if (size > (buf->size - buf->cur_pos)) {
		warning("Section is bigger than remaining space in file\n");
		buf->failed = 1;
		buf->cur_pos = buf->size;
		return;
	}
	if (section_id > buf->max_handler || !buf->handlers[section_id].fun) {
		warning("No handler for section ID %d, save state may be from a newer version\n", section_id);
		buf->cur_pos += size;
		return;
	}
	deserialize_buffer section;
	init_deserialize(&section, buf->data + buf->cur_pos, size);
	buf->handlers[section_id].fun(&section, buf->handlers[section_id].data);
	if (section.failed) {
		/* RetroPlug: a section's own short field is this buffer's refusal: the handler has written part of itself, so
		 * the state cannot be trusted and nothing further in it should be read. */
		buf->failed = 1;
		buf->cur_pos = buf->size;
		return;
	}
	buf->cur_pos += size;
}

static const char sz_ident[] = "BLSTSZ\x01\x07";

uint8_t save_to_file(serialize_buffer *buf, char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		return 0;
	}
	if (fwrite(sz_ident, 1, sizeof(sz_ident)-1, f) != sizeof(sz_ident)-1) {
		fclose(f);
		return 0;
	}
	if (fwrite(buf->data, 1, buf->size, f) != buf->size) {
		fclose(f);
		return 0;
	}
	fclose(f);
	return 1;
}

uint8_t load_from_file(deserialize_buffer *buf, char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return 0;
	}
	char ident[sizeof(sz_ident)-1];
	long size = file_size(f);
	if (size < sizeof(ident)) {
		fclose(f);
		return 0;
	}
	if (fread(ident, 1, sizeof(ident), f) != sizeof(ident)) {
		fclose(f);
		return 0;
	}
	if (memcmp(ident, sz_ident, sizeof(ident))) {
		return 0;
	}
	buf->size = size - sizeof(ident);
	buf->cur_pos = 0;
	buf->data = malloc(buf->size);
	buf->handlers = NULL;
	buf->max_handler = 8;
	if (fread(buf->data, 1, buf->size, f) != buf->size) {
		fclose(f);
		free(buf->data);
		buf->data = NULL;
		buf->size = 0;
		return 0;
	}
	fclose(f);
	return 1;
}
