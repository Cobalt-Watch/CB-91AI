# Host tests of the pure logic of firmware/lib: gcc and make, nothing else.
#
#   python firmware/tools/host_tests.py     on the bench PC (runs this in WSL)
#   make -C firmware/tests/host             on Linux, and in the CI of lot A5
#
# Built with the sanitizers: an overflow or an undefined behaviour fails a test
# even when the numbers come out right.

CC ?= gcc
CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Wshadow -Wstrict-prototypes -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all
LIB := ../../lib
WATCH := ../../watch/src
BUILD := build

TESTS := test_gesture test_calendar test_ui test_light test_link test_session test_vad test_note \
	test_store test_tlog test_take test_pair test_recall test_clicks test_stuck test_cell test_counts \
	test_daily test_look

.PHONY: test clean

test: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done
	@# The PC side of the link reads and writes the same vectors, the other way round
	@python3 ../../tools/cobalt_link.py --self-test

$(BUILD)/test_gesture: test_gesture.c harness.h $(LIB)/gesture.c $(LIB)/gesture.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -o $@ test_gesture.c $(LIB)/gesture.c

$(BUILD)/test_calendar: test_calendar.c harness.h $(LIB)/calendar.c $(LIB)/calendar.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -o $@ test_calendar.c $(LIB)/calendar.c

$(BUILD)/test_link: test_link.c harness.h $(LIB)/link_proto.c $(LIB)/link_proto.h \
		../vectors/link_v1.txt | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -o $@ test_link.c $(LIB)/link_proto.c

$(BUILD)/test_session: test_session.c harness.h $(WATCH)/session.c $(WATCH)/session.h \
		$(LIB)/link_proto.c $(LIB)/link_proto.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -I$(WATCH) -o $@ test_session.c $(WATCH)/session.c \
		$(LIB)/link_proto.c

$(BUILD)/test_ui: test_ui.c harness.h $(WATCH)/ui.c $(WATCH)/ui.h $(LIB)/calendar.c \
		$(LIB)/calendar.h $(LIB)/gesture.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -I$(WATCH) -o $@ test_ui.c $(WATCH)/ui.c $(LIB)/calendar.c

$(BUILD)/test_light: test_light.c harness.h $(WATCH)/light.c $(WATCH)/light.h \
		$(LIB)/link_proto.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -I$(WATCH) -o $@ test_light.c $(WATCH)/light.c

$(BUILD)/test_vad: test_vad.c harness.h $(LIB)/vad.c $(LIB)/vad.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -o $@ test_vad.c $(LIB)/vad.c -lm

$(BUILD)/test_note: test_note.c harness.h $(LIB)/note.c $(LIB)/note.h ../vectors/note_v2.txt \
		| $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -o $@ test_note.c $(LIB)/note.c

$(BUILD)/test_store: test_store.c harness.h $(WATCH)/store.c $(WATCH)/store.h $(LIB)/note.c \
		$(LIB)/note.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -I$(WATCH) -o $@ test_store.c $(WATCH)/store.c $(LIB)/note.c

$(BUILD)/test_tlog: test_tlog.c harness.h $(WATCH)/tlog.c $(WATCH)/tlog.h $(WATCH)/store.h \
		$(LIB)/note.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -I$(WATCH) -o $@ test_tlog.c $(WATCH)/tlog.c

$(BUILD)/test_take: test_take.c harness.h $(WATCH)/take.c $(WATCH)/take.h $(LIB)/note.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -I$(WATCH) -o $@ test_take.c $(WATCH)/take.c

$(BUILD)/test_pair: test_pair.c harness.h $(WATCH)/pair.c $(WATCH)/pair.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(WATCH) -o $@ test_pair.c $(WATCH)/pair.c

$(BUILD)/test_look: test_look.c harness.h $(WATCH)/look.c $(WATCH)/look.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(WATCH) -o $@ test_look.c $(WATCH)/look.c

$(BUILD)/test_recall: test_recall.c harness.h $(WATCH)/recall.c $(WATCH)/recall.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(WATCH) -o $@ test_recall.c $(WATCH)/recall.c

$(BUILD)/test_clicks: test_clicks.c harness.h $(WATCH)/clicks.c $(WATCH)/clicks.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(WATCH) -o $@ test_clicks.c $(WATCH)/clicks.c

$(BUILD)/test_stuck: test_stuck.c harness.h $(WATCH)/stuck.c $(WATCH)/stuck.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(WATCH) -o $@ test_stuck.c $(WATCH)/stuck.c

$(BUILD)/test_cell: test_cell.c harness.h $(WATCH)/cell.c $(WATCH)/cell.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(WATCH) -o $@ test_cell.c $(WATCH)/cell.c

$(BUILD)/test_counts: test_counts.c harness.h $(WATCH)/counts.c $(WATCH)/counts.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(WATCH) -o $@ test_counts.c $(WATCH)/counts.c

$(BUILD)/test_daily: test_daily.c harness.h $(WATCH)/daily.c $(WATCH)/daily.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(WATCH) -o $@ test_daily.c $(WATCH)/daily.c

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
