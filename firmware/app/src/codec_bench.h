/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CB91AI_CODEC_BENCH_H
#define CB91AI_CODEC_BENCH_H

#include <zephyr/shell/shell.h>

/*
 * `cb91ai codec ...`: cost of the candidate speech codecs on this board (lot
 * K2). Only built with CONFIG_CB91AI_CODEC_BENCH, see src/codec_bench.c.
 */
int codec_bench_cmd(const struct shell *sh, size_t argc, char **argv);

#endif /* CB91AI_CODEC_BENCH_H */
