/*
 * Cache Simulator
 *
 * This program implements a configurable set-associative cache simulator
 *
 * Major components:
 *   1) Command-line interface (parse_args):
 *      - Parses options via getopt:
 *          -h  help, -v verbose, -s <s>, -b <b>, -E <E>, -t <trace>
 *      - Validates numeric inputs using strtoul/strtoull:
 *          s and b must be non-negative integers; E must be a positive integer.
 *      - Ensures all required options (-s, -b, -E, -t) are present.
 *      - Performs a sanity check that (s + b) does not exceed the machine
 * address.
 *      - Rejects extra arguments
 *
 *   2) Cache model:
 *      - Parameters:
 *          s : set index bits
 *          b : block offset bits
 *          E : lines per set
 *
 *   3) Trace processing:
 *      - Reads the trace file line-by-line.
 *      - Parses only Load/Store operations
 *      - Decodes each address into tag/set/offset fields.
 *
 *   4) Simulation:
 *      - Replacement: LRU using a monotonically increasing timestamp.
 *      - Write policy:
 *          * Store operations mark the cache line dirty.
 *          * Evicting a dirty line accounts for writing back an entire block to
 * memory.
 *
 * Output statistics:
 *   hits, misses, evictions, dirty_bytes, dirty_evictions
 */

#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include "cachelab.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Commands:
 * Stores parsed command-line options:
 *   s : set index bits
 *   b : block offset bits
 *   E : lines per set
 *   t : trace file path
 * plus flags for help/verbose and whether required options were provided.
 */
typedef struct {
    bool help;
    bool verbose;
    bool s_set, b_set, E_set, t_set;
    unsigned long s;
    unsigned long b;
    unsigned long E;
    char t[PATH_MAX];
} Commands;

/*
 * Operations:
 * Stores one memory operation from the trace:
 *   opt   = 'L' or 'S'
 *   addr  = memory address
 *   size  = access size
 * plus decoded fields used by cache simulation:
 *   tag, set, offset
 */
typedef struct {
    char opt;
    unsigned long long addr;
    unsigned long long tag;
    unsigned long long set;
    unsigned long long offset;
    unsigned long size;
} Operations;

/*
 * Line:
 * One cache line:
 *   valid : whether the block is occupied
 *   dirty : whether the block is dirty
 *   tag   : tag of cached block
 *   last_used : timestamp for LRU replacement
 */
typedef struct {
    bool valid;
    bool dirty;
    unsigned long long tag;
    unsigned long long last_used;
} Line;

/*
 * Set:
 * One cache set is an array of E lines.
 */
typedef struct {
    Line *lines;
} Set;

/*
 * Cache:
 * Whole cache structure:
 *   sets : array length S (S = 2^s)
 *   E    : lines per set
 *   B    : block size in bytes (B = 2^b)
 */
typedef struct {
    Set *sets;
    unsigned long s;
    unsigned long b;
    unsigned long E;
    unsigned long S;
    unsigned long B;
} Cache;

// print basic message
static void printMessage(const char *prog) {
    printf("Usage: %s [-v] -s <s> -b <b> -E <E> -t <trace>\n", prog);
    printf("       %s -h\n\n", prog);
    printf("  -h          Print this help message and exit\n");
    printf("  -v          Verbose mode: report effects of each memory "
           "operation\n");
    printf("  -s <s>      Number of set index bits (there are 2**s sets)\n");
    printf("  -b <b>      Number of block bits (there are 2**b blocks)\n");
    printf("  -E <E>      Number of lines per set (associativity)\n");
    printf("  -t <trace>  File name of the memory trace to process\n\n");
    printf("The -s, -b, -E, and -t options must be supplied for all "
           "simulations.\n");
}

/*
 * parse_nonneg_ulong:
 * Parses a base-10 non-negative integer string into unsigned long.
 * Returns false if not purely numeric or overflow.
 * Used for s and b.
 */
static bool parse_nonneg_ulong(const char *str, unsigned long *out) {
    if (!str || *str == '\0')
        return false;

    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(str, &end, 10);

    if (end == str || *end != '\0')
        return false;
    if (errno == ERANGE)
        return false;

    *out = v;
    return true;
}

/*
 * parse_pos_ulong:
 * Same as parse_nonneg_ulong but must be >= 1.
 * Used for E.
 */
static bool parse_pos_ulong(const char *str, unsigned long *out) {
    if (!parse_nonneg_ulong(str, out))
        return false;
    return (*out > 0);
}

/*
 * parse_args:
 * Uses getopt to parse command line flags/options into Commands *cmd.
 *
 * Returns:
 *   0 on success
 *   1 on any error
 */
static int parse_args(int argc, char **argv, Commands *cmd) {
    opterr = 0;
    int opt;
    while ((opt = getopt(argc, argv, ":hvs:b:E:t:")) != -1) {
        switch (opt) {
        case 'h':
            cmd->help = true;
            return 0;
        case 'v':
            cmd->verbose = true;
            break;
        case 's':
            if (!parse_nonneg_ulong(optarg, &cmd->s)) {
                fprintf(stderr, "Error: -s must be a non-negative integer.\n");
                return 1;
            }
            cmd->s_set = true;
            break;
        case 'b':
            if (!parse_nonneg_ulong(optarg, &cmd->b)) {
                fprintf(stderr, "Error: -b must be a non-negative integer.\n");
                return 1;
            }
            cmd->b_set = true;
            break;
        case 'E':
            if (!parse_pos_ulong(optarg, &cmd->E)) {
                fprintf(stderr, "Error: -E must be a positive integer.\n");
                return 1;
            }
            cmd->E_set = true;
            break;
        case 't':
            if (!optarg || *optarg == '\0')
                return 1;
            strncpy(cmd->t, optarg, PATH_MAX - 1);
            cmd->t[PATH_MAX - 1] = '\0';
            cmd->t_set = true;
            break;
        case ':':
            return 1;
        default:
            return 1;
        }
    }

    if (!cmd->s_set || !cmd->b_set || !cmd->E_set || !cmd->t_set)
        return 1;

    unsigned long addr_bits = (unsigned long)(sizeof(uintptr_t) * 8);
    if (cmd->s + cmd->b > addr_bits)
        return 1;

    if (optind < argc)
        return 1;
    return 0;
}

/*
 * cache_init:
 * Allocates an empty cache and returns it by value.
 */
static Cache cache_init(const Commands *cmd) {
    Cache c;
    c.s = cmd->s;
    c.b = cmd->b;
    c.E = cmd->E;
    c.S = 1UL << cmd->s;
    c.B = 1UL << cmd->b;

    c.sets = (Set *)calloc(c.S, sizeof(Set));
    if (!c.sets) {
        perror("calloc sets");
        exit(1);
    }

    for (unsigned long i = 0; i < c.S; i++) {
        c.sets[i].lines = (Line *)calloc(c.E, sizeof(Line));
        if (!c.sets[i].lines) {
            perror("calloc lines");
            exit(1);
        }
    }
    return c;
}

/*
 * cache_free:
 * Frees memory allocated by cache_init.
 */
static void cache_free(Cache *c) {
    if (!c || !c->sets)
        return;
    for (unsigned long i = 0; i < c->S; i++)
        free(c->sets[i].lines);
    free(c->sets);
    c->sets = NULL;
}

/*
 * decode_addr:
 * Splits op->addr into:
 *   offset
 *   set
 *   tag
 */
static void decode_addr(const Commands *cmd, Operations *op) {
    unsigned long long offset_mask =
        (cmd->b == 0) ? 0ULL : ((1ULL << cmd->b) - 1ULL);
    unsigned long long set_mask =
        (cmd->s == 0) ? 0ULL : ((1ULL << cmd->s) - 1ULL);

    op->offset = op->addr & offset_mask;
    op->set = (cmd->s == 0) ? 0ULL : ((op->addr >> cmd->b) & set_mask);
    op->tag = op->addr >> (cmd->s + cmd->b);
}

/*
 * process_line:
 * Parse one trace line into an Operations struct.
 *
 * Behavior:
 *   - Skips leading whitespace
 *   - Requires opt to be 'L' or 'S' (ignores 'I' and other lines)
 *   - Reads address until comma (address parsed as hex, base 16)
 *   - Reads size after comma (parsed as base 10)
 *
 * Returns:
 *   0 on success
 *   1 on parse failure (caller will ignore the line)
 */
static int process_line(const char *linebuf, Operations *op) {
    const char *p = linebuf;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0' || *p == '\n')
        return 1;

    if (*p != 'L' && *p != 'S')
        return 1;
    op->opt = *p++;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return 1;

    char addr_str[64] = {0};
    int k = 0;
    while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\n') {
        if (k < (int)sizeof(addr_str) - 1)
            addr_str[k++] = *p;
        p++;
    }
    if (k == 0)
        return 1;

    errno = 0;
    char *end = NULL;
    op->addr = strtoull(addr_str, &end, 16);
    if (errno == ERANGE || end == addr_str)
        return 1;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != ',')
        return 1;
    p++;

    errno = 0;
    char *end2 = NULL;
    op->size = strtoul(p, &end2, 10);
    if (end2 == p)
        return 1;

    return 0;
}

/*
 * execute:
 * Simulate cache access for store or load action based on LRU principle

 * Updates:
 *   stats->hits, stats->misses, stats->evictions, stats->dirty_evictions
 */
static void execute(Cache *cache, csim_stats_t *stats, const Commands *cmd,
                    Operations *op, unsigned long long *time_counter) {
    (*time_counter)++;

    // Decode tag/set/offset from address
    decode_addr(cmd, op);

    Set *set = &cache->sets[op->set];

    // Hit check
    for (unsigned long i = 0; i < cache->E; i++) {
        Line *ln = &set->lines[i];
        // Hit if line is valid and tags match
        if (ln->valid && ln->tag == op->tag) {
            stats->hits++;
            // Update recency
            ln->last_used = *time_counter;
            // Store makes line dirty
            if (op->opt == 'S')
                ln->dirty = true;

            if (cmd->verbose) {
                printf("%c %llx,%lu hit\n", op->opt, op->addr, op->size);
            }
            return;
        }
    }

    // Miss handling
    stats->misses++;

    // Try to find an empty line first
    long victim = -1;
    for (unsigned long i = 0; i < cache->E; i++) {
        if (!set->lines[i].valid) {
            victim = (long)i;
            break;
        }
    }

    bool eviction = false;

    // If no empty line, evict LRU
    if (victim == -1) {
        eviction = true;
        stats->evictions++;

        unsigned long lru_i = 0;
        unsigned long long best = set->lines[0].last_used;
        for (unsigned long i = 1; i < cache->E; i++) {
            if (set->lines[i].last_used < best) {
                best = set->lines[i].last_used;
                lru_i = i;
            }
        }
        victim = (long)lru_i;

        // If victim is dirty, write back the entire block
        if (set->lines[victim].dirty) {
            stats->dirty_evictions += cache->B;
        }
    }

    // Fill chosen victim line with new block
    Line *ln = &set->lines[victim];
    ln->valid = true;
    ln->tag = op->tag;
    ln->last_used = *time_counter;
    // Store makes new line dirty; load leaves it clean
    ln->dirty = (op->opt == 'S');

    if (cmd->verbose) {
        if (!eviction)
            printf("%c %llx,%lu miss\n", op->opt, op->addr, op->size);
        else
            printf("%c %llx,%lu miss eviction\n", op->opt, op->addr, op->size);
    }
}

/*
 * process_trace_file:
 * Opens the trace file, reads line by line, parses each line, and simulates
 * each operation.
 *
 * Lines that do not match expected format are ignored.
 *
 * Returns:
 *   0 on success
 *   1 on file open error
 */
static int process_trace_file(const char *trace, Cache *cache, Commands *cmd,
                              csim_stats_t *stats) {
    FILE *tfp = fopen(trace, "rt");
    if (!tfp) {
        fprintf(stderr, "Error opening '%s': %s\n", trace, strerror(errno));
        return 1;
    }

    char linebuf[256];
    unsigned long long time_counter = 0;

    while (fgets(linebuf, sizeof(linebuf), tfp)) {
        Operations op = {0};
        if (process_line(linebuf, &op) != 0) {
            // Ignore lines that don't match
            continue;
        }
        // Simulate one access
        execute(cache, stats, cmd, &op, &time_counter);
    }

    fclose(tfp);
    return 0;
}

/*
 * compute_dirty_bytes:
 * After processing all traces, count how many valid lines are dirty, and
 * compute dirty_bytes.
 */
static void compute_dirty_bytes(Cache *cache, csim_stats_t *stats) {
    unsigned long dirty_blocks = 0;
    for (unsigned long i = 0; i < cache->S; i++) {
        for (unsigned long j = 0; j < cache->E; j++) {
            Line *ln = &cache->sets[i].lines[j];
            if (ln->valid && ln->dirty)
                dirty_blocks++;
        }
    }
    stats->dirty_bytes = dirty_blocks * cache->B;
}

/*
 * main:
 *  1) Parse args
 *  2) Initialize cache
 *  3) Process trace file
 *  4) Compute dirty bytes at end
 *  5) Print summary using cachelab's printSummary()
 *  6) Free memory
 */
int main(int argc, char **argv) {
    Commands cmd = {0};

    int rc = parse_args(argc, argv, &cmd);
    if (rc != 0) {
        printMessage(argv[0]);
        return 1;
    }

    if (cmd.help) {
        printMessage(argv[0]);
        return 0;
    }

    Cache cache = cache_init(&cmd);
    csim_stats_t stats = {0};

    if (process_trace_file(cmd.t, &cache, &cmd, &stats) != 0) {
        cache_free(&cache);
        return 1;
    }

    compute_dirty_bytes(&cache, &stats);
    printSummary(&stats);
    cache_free(&cache);
    return 0;
}
