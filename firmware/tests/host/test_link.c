/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the Cobalt Link messages (firmware/lib/link_proto.c, lot D6):
 * every vector of firmware/tests/vectors/link_v1.txt (path in argv[1]) is
 * written by the watch's encoder or read by its parser, depending on its way,
 * and must give the same bytes or the same fields; then what the parser must
 * refuse. The PC harness checks the same vectors the other way round.
 */

#define _DEFAULT_SOURCE /* strtok_r() */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "link_proto.h"

#define LINE_MAX_LEN 512
#define TOKENS_MAX   16

struct vector {
	char name[24];
	uint8_t bytes[64];
	size_t len;
	char *keys[TOKENS_MAX];
	char *values[TOKENS_MAX];
	size_t fields;
};

static size_t unhex(const char *hex, uint8_t *out, size_t room)
{
	size_t n = 0;

	while (hex[0] != '\0' && hex[1] != '\0' && n < room) {
		char pair[3] = { hex[0], hex[1], '\0' };

		out[n++] = (uint8_t)strtoul(pair, NULL, 16);
		hex += 2;
	}
	return n;
}

static bool parse_line(char *line, struct vector *v)
{
	char *save = NULL;
	char *tok = strtok_r(line, " \r\n", &save);

	memset(v, 0, sizeof(*v));
	if (tok == NULL || tok[0] == '#') {
		return false;
	}
	strncpy(v->name, tok, sizeof(v->name) - 1);
	tok = strtok_r(NULL, " \r\n", &save);
	if (tok == NULL) {
		return false;
	}
	v->len = unhex(tok, v->bytes, sizeof(v->bytes));
	while ((tok = strtok_r(NULL, " \r\n", &save)) != NULL && v->fields < TOKENS_MAX) {
		char *eq = strchr(tok, '=');

		if (eq != NULL) {
			*eq = '\0';
			v->keys[v->fields] = tok;
			v->values[v->fields] = eq + 1;
			v->fields++;
		}
	}
	return true;
}

static const char *field(const struct vector *v, const char *key)
{
	for (size_t i = 0; i < v->fields; i++) {
		if (strcmp(v->keys[i], key) == 0) {
			return v->values[i];
		}
	}
	printf("  %s: no field %s\n", v->name, key);
	harness_failures++;
	return "0";
}

static long long num(const struct vector *v, const char *key)
{
	return strtoll(field(v, key), NULL, 10);
}

static void check_bytes(const struct vector *v, const uint8_t *got, size_t len)
{
	harness_checks++;
	if (len != v->len || memcmp(got, v->bytes, len) != 0) {
		harness_failures++;
		printf("  %s: %zu byte(s) written, %zu expected, or different bytes\n", v->name, len,
		       v->len);
	}
}

/* Watch to phone: the encoder must write the vector's bytes */
static void encode(const struct vector *v)
{
	uint8_t buf[64];
	size_t len = 0;

	if (strcmp(v->name, "HELLO") == 0) {
		const struct link_hello m = {
			.version = (uint8_t)num(v, "version"),
			.reason = (uint8_t)num(v, "reason"),
			.state = (uint8_t)num(v, "state"),
			.fw_major = (uint8_t)num(v, "fw_major"),
			.fw_minor = (uint8_t)num(v, "fw_minor"),
			.fw_revision = (uint16_t)num(v, "fw_revision"),
			.notes = (uint8_t)num(v, "notes"),
			.note_bytes = (uint32_t)num(v, "note_bytes"),
			.battery_mv = (uint16_t)num(v, "battery_mv"),
			.codecs = (uint8_t)num(v, "codecs"),
			.watch_id = (uint32_t)num(v, "watch_id"),
		};

		len = link_put_hello(buf, sizeof(buf), &m);
		/* Every message but the data fits the smallest MTU */
		CHECK(len <= LINK_SMALL_MAX);
	} else if (strcmp(v->name, "NOTE_OFFER") == 0) {
		len = link_put_note_offer(buf, sizeof(buf), (uint32_t)num(v, "id"),
					  (uint32_t)num(v, "size"), (uint32_t)num(v, "crc32"));
	} else if (strcmp(v->name, "NOTE_DATA") == 0) {
		uint8_t data[32];
		const size_t n = unhex(field(v, "data"), data, sizeof(data));

		len = link_put_note_data(buf, sizeof(buf));
		memcpy(&buf[len], data, n);
		len += n;
	} else if (strcmp(v->name, "NOTE_END") == 0) {
		len = link_put_note_end(buf, sizeof(buf), (uint32_t)num(v, "id"));
	} else if (strcmp(v->name, "VALUE") == 0) {
		uint8_t value[32];
		const size_t n = unhex(field(v, "value"), value, sizeof(value));

		len = link_put_value(buf, sizeof(buf), (uint8_t)num(v, "key"),
				     (uint8_t)num(v, "status"), value, n);
		if (num(v, "key") == LINK_KEY_COUNTERS) {
			/* The counters, written by their own writer, give the same value */
			const struct link_counters c = { 42, 630, 95, 7, 1, 0 };
			uint8_t mine[LINK_COUNTERS_SIZE];

			CHECK_EQ(link_put_counters(mine, sizeof(mine), &c), LINK_COUNTERS_SIZE);
			CHECK(n == LINK_COUNTERS_SIZE && memcmp(mine, value, n) == 0);
		}
		if (num(v, "key") == LINK_KEY_DEBUG_TRIAL) {
			/* The trial of the calls, written by its own writer */
			const struct link_trial t = { 87, LINK_TRIAL_APPLE, 12, 2, 1834, 2410,
						      24, 0, 400 };
			uint8_t mine[LINK_TRIAL_SIZE];

			CHECK_EQ(link_put_trial(mine, sizeof(mine), &t), LINK_TRIAL_SIZE);
			CHECK_EQ(link_put_trial(mine, LINK_TRIAL_SIZE - 1, &t), 0);
			CHECK(n == LINK_TRIAL_SIZE && memcmp(mine, value, n) == 0);
		}
		if (num(v, "key") == LINK_KEY_JOURNAL) {
			/* A page of the journal, written by its own writer */
			const struct link_journal_record r[2] = {
				{ 1790194800U, 2175, 2903 },
				{ 1790195400U, -325, 0x8000U | 2851U },
			};
			uint8_t mine[32];
			size_t put;

			CHECK_EQ(link_put_journal(mine, sizeof(mine), 0xC0B91AA1U, 1020, r, 2, &put),
				 LINK_JOURNAL_HEAD + 2U * LINK_JOURNAL_RECORD);
			CHECK_EQ(put, 2);
			CHECK(n == LINK_JOURNAL_HEAD + 2U * LINK_JOURNAL_RECORD &&
			      memcmp(mine, value, n) == 0);
		}
	} else if (strcmp(v->name, "EVENT") == 0) {
		len = link_put_event(buf, sizeof(buf), (uint8_t)num(v, "gesture"),
				     (uint8_t)num(v, "button"));
	} else if (strcmp(v->name, "BYE_WATCH") == 0) {
		len = link_put_bye(buf, sizeof(buf), (uint8_t)num(v, "reason"));
	} else {
		printf("  %s: not a message of the watch\n", v->name);
		harness_failures++;
		return;
	}
	check_bytes(v, buf, len);
}

/* Phone to watch: the parser must give the vector's fields */
static void parse(const struct vector *v)
{
	struct link_msg m;

	CHECK_EQ(link_parse(v->bytes, v->len, &m), LINK_PARSED);
	CHECK_EQ(m.type, v->bytes[0]);
	if (strcmp(v->name, "TIME") == 0) {
		CHECK_EQ(m.u.time.utc_ms, num(v, "utc_ms"));
		CHECK_EQ(m.u.time.tz_minutes, num(v, "tz_minutes"));
	} else if (strcmp(v->name, "NOTE_ACCEPT") == 0) {
		CHECK_EQ(m.u.accept.id, num(v, "id"));
		CHECK_EQ(m.u.accept.offset, num(v, "offset"));
	} else if (strcmp(v->name, "NOTE_ACK") == 0) {
		CHECK_EQ(m.u.ack.id, num(v, "id"));
		CHECK_EQ(m.u.ack.verdict, num(v, "verdict"));
		CHECK_EQ(m.u.ack.window_s, num(v, "window_s"));
	} else if (strcmp(v->name, "RESULT") == 0) {
		CHECK_EQ(m.u.result.id, num(v, "id"));
		CHECK_EQ(m.u.result.code, num(v, "code"));
	} else if (strcmp(v->name, "SET") == 0) {
		uint8_t value[32];
		const size_t n = unhex(field(v, "value"), value, sizeof(value));

		CHECK_EQ(m.u.set.key, num(v, "key"));
		CHECK_EQ(m.u.set.len, n);
		CHECK(memcmp(m.u.set.value, value, n) == 0);
	} else if (strcmp(v->name, "GET") == 0) {
		CHECK_EQ(m.u.get.key, num(v, "key"));
	} else if (strcmp(v->name, "BYE_PHONE") == 0) {
		CHECK_EQ(m.u.bye.reason, num(v, "reason"));
	} else {
		printf("  %s: not a message of the phone\n", v->name);
		harness_failures++;
	}
}

static const char *vectors_path;

static void every_vector(void)
{
	FILE *f = fopen(vectors_path, "r");
	char line[LINE_MAX_LEN];
	struct vector v;
	unsigned int seen = 0;

	CHECK(f != NULL);
	if (f == NULL) {
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		if (!parse_line(line, &v)) {
			continue;
		}
		seen++;
		if (v.len > 0 && (v.bytes[0] & LINK_TO_WATCH) != 0) {
			parse(&v);
		} else {
			encode(&v);
		}
	}
	fclose(f);
	/* All the types of version 1, both ways */
	CHECK(seen >= 14);
}

static void refused_messages(void)
{
	struct link_msg m;
	const uint8_t hello[] = { LINK_HELLO, 1 };
	const uint8_t future[] = { 0xc0, 1, 2, 3 };
	const uint8_t short_time[] = { LINK_TIME, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	const uint8_t short_get[] = { LINK_GET };

	CHECK_EQ(link_parse(hello, 0, &m), LINK_EMPTY);
	CHECK_EQ(link_parse(hello, sizeof(hello), &m), LINK_WRONG_WAY);
	CHECK_EQ(link_parse(future, sizeof(future), &m), LINK_UNKNOWN);
	CHECK_EQ(link_parse(short_time, sizeof(short_time), &m), LINK_SHORT);
	CHECK_EQ(link_parse(short_get, sizeof(short_get), &m), LINK_SHORT);
}

static void later_versions_append(void)
{
	/* A RESULT of a later version, with two more bytes: they are ignored */
	const uint8_t result[] = { LINK_RESULT, 12, 0, 0, 0, 0, 0xaa, 0xbb };
	/* A window longer than the watch waits is capped */
	const uint8_t ack[] = { LINK_NOTE_ACK, 1, 0, 0, 0, 0, 200 };
	/* A SET value longer than a small message is cut */
	uint8_t set[40] = { LINK_SET, LINK_KEY_STATUS };
	struct link_msg m;

	CHECK_EQ(link_parse(result, sizeof(result), &m), LINK_PARSED);
	CHECK_EQ(m.u.result.id, 12);
	CHECK_EQ(m.u.result.code, 0);
	CHECK_EQ(link_parse(ack, sizeof(ack), &m), LINK_PARSED);
	CHECK_EQ(m.u.ack.window_s, LINK_WINDOW_MAX_S);
	CHECK_EQ(link_parse(set, sizeof(set), &m), LINK_PARSED);
	CHECK_EQ(m.u.set.len, LINK_SMALL_MAX - 2);
}

static void writers_refuse_small_buffers(void)
{
	uint8_t buf[64];
	const struct link_hello h = { 0 };
	const struct link_counters c = { 0 };

	CHECK_EQ(link_put_hello(buf, 19, &h), 0);
	CHECK_EQ(link_put_note_offer(buf, 12, 1, 2, 3), 0);
	CHECK_EQ(link_put_note_data(buf, 1), 0);
	CHECK_EQ(link_put_note_end(buf, 4, 1), 0);
	CHECK_EQ(link_put_value(buf, 5, 1, 0, buf, 3), 0);
	CHECK_EQ(link_put_event(buf, 2, 1, 1), 0);
	CHECK_EQ(link_put_bye(buf, 1, 0), 0);
	CHECK_EQ(link_put_counters(buf, LINK_COUNTERS_SIZE - 1, &c), 0);
}

/* A page of the journal takes what the room and LINK_JOURNAL_MAX let it */
static void journal_pages(void)
{
	static struct link_journal_record r[40];
	uint8_t buf[300];
	size_t put;

	for (size_t i = 0; i < 40; i++) {
		r[i] = (struct link_journal_record){ 1790194800U + 600U * (uint32_t)i, 2000, 2900 };
	}
	CHECK_EQ(link_put_journal(buf, LINK_JOURNAL_HEAD - 1U, 1, 0, r, 40, &put), 0);
	CHECK_EQ(put, 0);
	/* The head alone: an empty page, the phone has everything */
	CHECK_EQ(link_put_journal(buf, sizeof(buf), 1, 7, r, 0, &put), LINK_JOURNAL_HEAD);
	CHECK_EQ(buf[8], 0);
	/* The smallest MTU, 23: a VALUE of 20 bytes, one record */
	CHECK_EQ(link_put_journal(buf, 20 - 3, 1, 7, r, 40, &put), LINK_JOURNAL_HEAD + 8U);
	CHECK_EQ(put, 1);
	/* A common MTU of 185: 21 records */
	CHECK_EQ(link_put_journal(buf, 185 - 6, 1, 7, r, 40, &put), LINK_JOURNAL_HEAD + 21U * 8U);
	CHECK_EQ(put, 21);
	CHECK_EQ(buf[8], 21);
	/* The largest VALUE of the watch: 29, and never more */
	CHECK_EQ(link_put_journal(buf, 241, 1, 7, r, 40, &put), LINK_JOURNAL_HEAD + 29U * 8U);
	CHECK_EQ(put, LINK_JOURNAL_MAX);
	CHECK_EQ(link_put_journal(buf, sizeof(buf), 1, 7, r, 40, &put), LINK_JOURNAL_HEAD + 29U * 8U);
	CHECK_EQ(put, LINK_JOURNAL_MAX);
}

static void names(void)
{
	/* Two names of watches, 2026-09-25 */
	CHECK(link_name_valid((const uint8_t *)"CB91 Office", 11));
	CHECK(link_name_valid((const uint8_t *)"CB91 Home", 9));
	CHECK(link_name_valid((const uint8_t *)"CB-91AI", 7));
	CHECK(link_name_valid((const uint8_t *)"x", 1));
	/* Too long for the scan response, empty, spaces at an end */
	CHECK(!link_name_valid((const uint8_t *)"CB-91AI du bureau", 17));
	CHECK(!link_name_valid((const uint8_t *)"CB91 Office!", 12));
	CHECK(!link_name_valid((const uint8_t *)"", 0));
	CHECK(!link_name_valid((const uint8_t *)" CB91", 5));
	CHECK(!link_name_valid((const uint8_t *)"CB91 ", 5));
	/* Printable ASCII only: no control, no byte of UTF-8 */
	CHECK(!link_name_valid((const uint8_t *)"CB\t91", 5));
	CHECK(!link_name_valid((const uint8_t *)"Montre \xc3\xa9", 9));
	CHECK(!link_name_valid((const uint8_t *)"CB\x7f", 3));
	CHECK(link_name_valid((const uint8_t *)"~ !", 3));
}

int main(int argc, char **argv)
{
	vectors_path = argc > 1 ? argv[1] : "../vectors/link_v1.txt";
	RUN(every_vector);
	RUN(refused_messages);
	RUN(later_versions_append);
	RUN(writers_refuse_small_buffers);
	RUN(journal_pages);
	RUN(names);
	return harness_report("link");
}
