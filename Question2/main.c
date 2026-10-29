/*
 * ===========================================================================
 *  Dynamic Text Mining and Linguistic Pattern Analyzer   -   main.c
 *
 *  Build : gcc -Wall -Wextra -std=c11 -o analyzer main.c
 *
 *  ------------------------------------------------------------------------
 *  WORD RULES  (documented here and in custom_analysis_explanation.md;
 *  applied identically by counting, statistics, sorting, search, the
 *  summary and the custom analysis)
 *
 *  1. A word is a run of ASCII letters and digits.  Bytes >= 0x80 (any
 *     non-ASCII byte) are treated as separators.
 *  2. Letters are converted to lower case before the word is stored, so
 *     "The" and "the" are the same word.
 *  3. Punctuation at the start or end of a word is stripped
 *     ( . , ; : ! ? quotes brackets ... ).
 *  4. An apostrophe inside a word is kept, so "don't" is one word.
 *     Leading and trailing apostrophes are stripped (they separate).
 *  5. A single hyphen inside a word is kept, so "brother-in-law" and
 *     "good-looking" are one word each.  A double hyphen "--", or a
 *     hyphen with a space around it, acts as a separator.
 *  6. Repeated whitespace (spaces, tabs, newlines) is one separator.
 *  7. A word longer than 39 characters is truncated to its first 39
 *     characters when it is stored in struct wordStat, and truncated_words
 *     is incremented.  longest_word() still reports the true, untruncated
 *     length, because it walks the raw text instead of the stats array.
 *  8. Average word length uses the true (untruncated) word length.
 *
 *  ------------------------------------------------------------------------
 *  MEMORY MODEL
 *  - text: malloc for the first block, realloc to double the capacity while
 *    text is read in fgets chunks, so text of any length can be pasted.
 *  - stats: malloc for the first block, realloc to double when the array of
 *    distinct words is full.
 *  - every realloc goes through a temporary pointer, every allocation result
 *    is checked, and a failed realloc never loses the original pointer.
 *  - everything is freed on every exit path (normal exit and error paths).
 *  - no global mutable state: all state lives in the TextAnalyzer struct,
 *    which every operation receives as a pointer.
 *  - set TEXTAN_TRACE=1 to print a line on each realloc of either buffer
 *    (used by the growing-input test).
 * ===========================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* ---- configuration ------------------------------------------------------ */
#define STUDENT_ID      "12345"
#define PROJECT_NAME    "Dynamic Text Mining and Linguistic Pattern Analyzer"
#define SOURCE_DEFAULT  "Source: Pride and Prejudice by Jane Austen (1813), Chapter 3. Project Gutenberg eBook #1342 (public domain)"
#define SOURCE_USER     "Source: User-supplied text"
#define CUSTOM_ANALYSIS "Custom Analysis: Lexical Diversity Score"

#define WORD_MAX        39      /* longest word that fits name[40] with NUL */
#define TEXT_START_CAP  256     /* bytes, first text allocation              */
#define STATS_START_CAP 16      /* entries, first stats allocation           */
#define INPUT_CHUNK     1024    /* fgets chunk size while reading text       */
#define LINE_BUF        128     /* menu / sub-menu / search lines            */
#define ECHO_LIMIT      4096    /* scripted input echoed only below this     */

/* ---- data structures ---------------------------------------------------- */
struct wordStat {
    char name[40];
    int  count;
};

typedef struct {
    char   *text;             /* heap block holding the text                */
    size_t  text_len;         /* characters used                            */
    size_t  text_cap;         /* characters allocated                       */
    struct wordStat *stats;   /* heap block holding distinct words          */
    size_t  distinct_count;   /* entries used                               */
    size_t  stats_cap;        /* entries allocated                          */
    size_t  total_words;
    size_t  truncated_words;
    size_t  total_letters;    /* true lengths, for the average              */
    int     stats_dirty;      /* text changed since the last analysis       */
    int     source_is_user;   /* 0 = default Austen paragraph               */
    int     has_search;
    char    last_search[WORD_MAX + 1];
    int     running;          /* menu loop flag, cleared by option 0        */
} TextAnalyzer;

typedef void (*Operation)(TextAnalyzer *a);

/* ---- small helpers ------------------------------------------------------ */
static int trace_on(void)
{
    const char *v = getenv("TEXTAN_TRACE");
    return (v != NULL && v[0] != '\0' && strcmp(v, "0") != 0);
}

/* Letters and digits only; every other byte (punctuation, whitespace and
   any byte >= 0x80, i.e. every non-ASCII byte) is a separator. */
static int is_word_byte(unsigned char c)
{
    return (c < 0x80) && isalnum(c);
}

/* Walk forward from p and return a pointer past the next word.
   Sets *start to the first byte of the word and *len to its true length.
   Returns NULL when no word remains at or after p. */
static const char *next_word(const char *p, const char **start, size_t *len)
{
    while (*p != '\0' && !is_word_byte((unsigned char)*p))
        p++;
    if (*p == '\0')
        return NULL;
    *start = p;
    while (*p != '\0') {
        unsigned char c = (unsigned char)*p;
        if (is_word_byte(c)) {              /* normal letter or digit   */
            p++;
            continue;
        }
        /* apostrophe or single hyphen kept only when it sits inside a word */
        if ((c == '\'' || c == '-') && is_word_byte((unsigned char)*(p + 1))) {
            p++;
            continue;
        }
        break;                              /* "--", " - ", punctuation, ... */
    }
    *len = (size_t)(p - *start);
    return p;
}

/* Grow a char buffer: malloc for the first block, else realloc (doubling).
   Uses a temporary pointer so the original is never lost on failure. */
static int text_reserve(char **buf, size_t *cap, size_t needed, const char *what)
{
    size_t newcap;
    char  *tmp;

    if (needed <= *cap)
        return 1;
    newcap = (*cap == 0) ? TEXT_START_CAP : *cap;
    while (newcap < needed) {
        if (newcap > ((size_t)-1) / 2) {    /* overflow guard */
            newcap = needed;
            break;
        }
        newcap *= 2;
    }
    if (*buf == NULL)
        tmp = malloc(newcap);
    else
        tmp = realloc(*buf, newcap);
    if (tmp == NULL) {
        fprintf(stderr,
                "Error: out of memory while growing the %s "
                "(needed %zu bytes).\n", what, needed);
        return 0;                           /* *buf untouched */
    }
    if (trace_on()) {
        fflush(stdout);
        fprintf(stderr, "[trace] %s capacity: %zu -> %zu bytes (needed %zu)\n",
                what, *cap, newcap, needed);
    }
    *buf = tmp;
    *cap = newcap;
    return 1;
}

/* Grow the word statistics array: malloc first, then realloc (doubling). */
static int stats_reserve(TextAnalyzer *a)
{
    size_t           newcap, old_bytes, new_bytes;
    struct wordStat *tmp;

    if (a->distinct_count < a->stats_cap)
        return 1;
    newcap = (a->stats_cap == 0) ? STATS_START_CAP : a->stats_cap * 2;
    if (newcap <= a->stats_cap ||
        newcap > ((size_t)-1) / sizeof *a->stats) {
        fprintf(stderr, "Error: word statistics capacity overflow.\n");
        return 0;
    }
    old_bytes = a->stats_cap * sizeof *a->stats;
    new_bytes = newcap * sizeof *a->stats;
    if (a->stats == NULL)
        tmp = malloc(new_bytes);
    else
        tmp = realloc(a->stats, new_bytes);
    if (tmp == NULL) {
        fprintf(stderr,
                "Error: out of memory while growing the word statistics "
                "array (%zu entries).\n", newcap);
        return 0;                           /* a->stats untouched */
    }
    if (trace_on()) {
        fflush(stdout);
        fprintf(stderr,
                "[trace] stats array capacity: %zu -> %zu entries "
                "(%zu -> %zu bytes)\n",
                a->stats_cap, newcap, old_bytes, new_bytes);
    }
    a->stats = tmp;
    a->stats_cap = newcap;
    return 1;
}

/* Store one word (lower-cased, truncated to 39 chars) into the stats. */
static void add_word(TextAnalyzer *a, const char *start, size_t len)
{
    char   word[WORD_MAX + 1];
    size_t n, i;

    n = (len > (size_t)WORD_MAX) ? (size_t)WORD_MAX : len;
    for (i = 0; i < n; i++)
        word[i] = (char)tolower((unsigned char)start[i]);
    word[n] = '\0';
    if (len > (size_t)WORD_MAX)
        a->truncated_words++;

    for (i = 0; i < a->distinct_count; i++) {
        if (strcmp(a->stats[i].name, word) == 0) {
            a->stats[i].count++;
            return;
        }
    }
    if (!stats_reserve(a))
        return;                             /* reported inside stats_reserve */
    strcpy(a->stats[a->distinct_count].name, word);   /* strlen <= 39 */
    a->stats[a->distinct_count].count = 1;
    a->distinct_count++;
}

/* Re-tokenise the whole text and rebuild the statistics. */
static void analyze(TextAnalyzer *a)
{
    const char *p, *start;
    size_t      len;

    a->total_words = 0;
    a->truncated_words = 0;
    a->total_letters = 0;
    a->distinct_count = 0;                  /* capacity is kept */
    a->stats_dirty = 0;
    if (a->text == NULL)
        return;

    p = a->text;
    while ((p = next_word(p, &start, &len)) != NULL) {
        add_word(a, start, len);
        a->total_words++;
        a->total_letters += len;
    }
}

static void ensure_stats(TextAnalyzer *a)
{
    if (a->stats_dirty)
        analyze(a);
}

static int require_text(const TextAnalyzer *a)
{
    if (a->text == NULL || a->text_len == 0) {
        printf("No text loaded yet - choose option 1 (default paragraph) "
               "or option 2 (your own text) first.\n");
        return 0;
    }
    return 1;
}

/* Non-zero when stdin is a pipe/file: piped input is not echoed by a
   terminal, so scripted runs echo the value after each short prompt. */
static int input_is_piped(void)
{
    return !isatty(fileno(stdin));
}

/* Read one line; returns 0 on EOF/error.  Echoes when input is piped. */
static int read_line(char *buf, size_t size)
{
    size_t len;
    int    ch;

    if (fgets(buf, (int)size, stdin) == NULL)
        return 0;
    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    } else {                                /* over-long line: drop the rest */
        while ((ch = getchar()) != '\n' && ch != EOF)
            ;
    }
    if (input_is_piped())
        printf("%s\n", buf);
    return 1;
}

/* Parse a whole line as an integer; returns 1 when the line is exactly one. */
static int parse_int(const char *s, long *out)
{
    char *end;
    long  v;

    while (isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return 0;
    v = strtol(s, &end, 10);
    while (isspace((unsigned char)*end))
        end++;
    if (end == s || *end != '\0')
        return 0;
    *out = v;
    return 1;
}

static void print_header(int source_is_user)
{
    printf("======================================================================\n");
    printf("Student ID: %s\n", STUDENT_ID);
    printf("Project: %s\n", PROJECT_NAME);
    printf("%s\n", source_is_user ? SOURCE_USER : SOURCE_DEFAULT);
    printf("%s\n", CUSTOM_ANALYSIS);
    printf("======================================================================\n");
}

/* ---- metrics ------------------------------------------------------------ */
typedef struct {
    double lds;              /* distinct / total * 100                    */
    double repetition;       /* repeated distinct / distinct * 100        */
    double hapax;            /* once-only distinct / distinct * 100       */
    double avg_len;          /* letters / total                           */
    size_t repeated_count;
    size_t hapax_count;
} Metrics;

static void compute_metrics(const TextAnalyzer *a, Metrics *m)
{
    size_t i;

    memset(m, 0, sizeof *m);
    if (a->total_words > 0)
        m->avg_len = (double)a->total_letters / (double)a->total_words;
    if (a->total_words == 0 || a->distinct_count == 0)
        return;                             /* no division by zero */
    m->lds = (double)a->distinct_count / (double)a->total_words * 100.0;
    for (i = 0; i < a->distinct_count; i++) {
        if (a->stats[i].count >= 2)
            m->repeated_count++;
        if (a->stats[i].count == 1)
            m->hapax_count++;
    }
    m->repetition = (double)m->repeated_count /
                    (double)a->distinct_count * 100.0;
    m->hapax = (double)m->hapax_count / (double)a->distinct_count * 100.0;
}

static const char *lds_band(double lds)
{
    if (lds >= 75.0)
        return "very varied vocabulary";
    if (lds >= 55.0)
        return "moderately varied";
    return "repetitive";
}

static const char *find_longest(const TextAnalyzer *a, size_t *len)
{
    const char *p, *start, *best = NULL;
    size_t      n, best_len = 0;

    *len = 0;
    if (a->text == NULL)
        return NULL;
    p = a->text;
    while ((p = next_word(p, &start, &n)) != NULL) {
        if (n > best_len) {
            best_len = n;
            best = start;
        }
    }
    if (best != NULL)
        *len = best_len;
    return best;
}

static size_t most_frequent_index(const TextAnalyzer *a)
{
    size_t i, best = 0;

    for (i = 1; i < a->distinct_count; i++) {
        if (a->stats[i].count > a->stats[best].count ||
            (a->stats[i].count == a->stats[best].count &&
             strcmp(a->stats[i].name, a->stats[best].name) < 0))
            best = i;
    }
    return best;
}

/* The entry shown by the memory inspection: last searched word if any,
   otherwise the most frequent word. */
static int selected_entry(const TextAnalyzer *a, size_t *index)
{
    size_t i;

    if (a->stats == NULL || a->distinct_count == 0)
        return 0;
    if (a->has_search) {
        for (i = 0; i < a->distinct_count; i++) {
            if (strcmp(a->stats[i].name, a->last_search) == 0) {
                *index = i;
                return 1;
            }
        }
    }
    *index = most_frequent_index(a);
    return 1;
}

static void print_memory_details(const TextAnalyzer *a)
{
    size_t   idx = 0;
    int      have;
    ptrdiff_t diff;

    have = selected_entry(a, &idx);

    printf("  Text buffer address         : %p\n", (void *)a->text);
    if (a->text != NULL)
        printf("      -> heap block holding the text "
               "(%zu bytes used, %zu bytes allocated)\n",
               a->text_len, a->text_cap);
    else
        printf("      -> no heap block yet: no text has been loaded\n");

    printf("  Word statistics array       : %p\n", (void *)a->stats);
    if (a->stats != NULL)
        printf("      -> heap block holding the array of distinct words "
               "(%zu entries used, %zu entries allocated, %zu bytes "
               "allocated)\n",
               a->distinct_count, a->stats_cap,
               a->stats_cap * sizeof(struct wordStat));
    else
        printf("      -> no heap block yet: no text has been analysed\n");

    if (have) {
        printf("  Selected word-stat entry    : %p  (word \"%s\")\n",
               (void *)&a->stats[idx], a->stats[idx].name);
        printf("      -> address of one element inside that array: "
               "base address + index * sizeof(struct wordStat)\n");
        diff = (char *)&a->stats[idx] - (char *)a->stats;
        printf("      -> index %zu, sizeof(struct wordStat) = %zu bytes; "
               "difference from base = %td bytes (expected %zu)\n",
               idx, sizeof(struct wordStat), diff,
               idx * sizeof(struct wordStat));
        printf("      -> sanity check: %s\n",
               (diff == (ptrdiff_t)(idx * sizeof(struct wordStat)))
                   ? "matches" : "MISMATCH");
    } else {
        printf("  Selected word-stat entry    : (none)\n");
        printf("      -> the array is empty, so no element can be selected\n");
    }
}

/* ---- comparators for qsort --------------------------------------------- */
static int cmp_alpha(const void *x, const void *y)
{
    const struct wordStat *a = (const struct wordStat *)x;
    const struct wordStat *b = (const struct wordStat *)y;

    return strcmp(a->name, b->name);
}

static int cmp_freq(const void *x, const void *y)
{
    const struct wordStat *a = (const struct wordStat *)x;
    const struct wordStat *b = (const struct wordStat *)y;

    if (a->count != b->count)
        return (a->count > b->count) ? -1 : 1;
    return strcmp(a->name, b->name);        /* ties broken alphabetically */
}

/* ---- default paragraph -------------------------------------------------- */
static const char DEFAULT_PARAGRAPH[] =
    "Mr. Bingley was good-looking and gentlemanlike; he had a pleasant countenance, "
    "and easy, unaffected manners. His sisters were fine women, with an air of "
    "decided fashion. His brother-in-law, Mr. Hurst, merely looked the gentleman; "
    "but his friend Mr. Darcy soon drew the attention of the room by his fine, "
    "tall person, handsome features, noble mien, and the report which was in "
    "general circulation within five minutes after his entrance, of his having "
    "ten thousand a year. The gentlemen pronounced him to be a fine figure of a "
    "man, the ladies declared he was much handsomer than Mr. Bingley, and he was "
    "looked at with great admiration for about half the evening, till his manners "
    "gave a disgust which turned the tide of his popularity; for he was discovered "
    "to be proud; to be above his company, and above being pleased; and not all "
    "his large estate in Derbyshire could then save him from having a most "
    "forbidding, disagreeable countenance, and being unworthy to be compared with "
    "his friend.";

/* ---- menu operations ---------------------------------------------------- */
static void use_default_text(TextAnalyzer *a)
{
    size_t n = strlen(DEFAULT_PARAGRAPH);
    char  *copy = malloc(n + 1);

    if (copy == NULL) {
        fprintf(stderr, "Error: out of memory loading the default "
                        "paragraph (%zu bytes).\n", n + 1);
        return;                             /* previous text kept */
    }
    memcpy(copy, DEFAULT_PARAGRAPH, n + 1);
    free(a->text);
    a->text = copy;
    a->text_len = n;
    a->text_cap = n + 1;
    a->source_is_user = 0;
    a->stats_dirty = 1;
    a->has_search = 0;
    printf("Loaded the default paragraph: %zu characters (heap block "
           "allocated with malloc, %zu bytes).\n", n, a->text_cap);
    printf("Choose option 3 to count the words.\n");
}

static void input_custom_text(TextAnalyzer *a)
{
    char   chunk[INPUT_CHUNK];
    char   preview_buf[61];
    char  *buf = NULL;
    size_t len = 0, cap = 0;
    size_t i, preview;
    int    eof = 0, ok = 1, echoed = 0;

    printf("Paste or type your text (any length).\n");
    printf("End with a line containing only END, or with EOF.\n");

    while (ok && !eof) {
        size_t clen, body;
        int    complete;

        if (fgets(chunk, (int)sizeof chunk, stdin) == NULL) {
            if (ferror(stdin))
                fprintf(stderr, "Error: read failed; keeping the previous "
                                "text.\n");
            eof = 1;
            break;
        }
        clen = strlen(chunk);
        complete = (clen > 0 && chunk[clen - 1] == '\n');
        body = complete ? clen - 1 : clen;
        if (body == 3 && strncmp(chunk, "END", 3) == 0)
            break;                          /* end of input marker */
        if (input_is_piped() && len + clen <= ECHO_LIMIT)
            printf("%s", chunk);            /* echo short scripted input only */
        else if (input_is_piped() && len + clen > ECHO_LIMIT && !echoed) {
            printf("... (input echo stopped: pasted text longer than %d "
                   "bytes) ...\n", ECHO_LIMIT);
            echoed = 1;
        }
        if (!text_reserve(&buf, &cap, len + clen + 1, "text buffer")) {
            ok = 0;
            break;
        }
        memcpy(buf + len, chunk, clen);
        len += clen;
        buf[len] = '\0';
    }

    if (!ok || (len == 0)) {
        free(buf);
        printf("Empty input: no text entered. The previous text is kept.\n");
        return;
    }
    for (i = 0; i < len; i++) {
        if (!isspace((unsigned char)buf[i]))
            break;
    }
    if (i == len) {
        free(buf);
        printf("Empty input: only whitespace was entered. "
               "The previous text is kept.\n");
        return;
    }

    free(a->text);
    a->text = buf;
    a->text_len = len;
    a->text_cap = cap;
    a->source_is_user = 1;
    a->stats_dirty = 1;
    a->has_search = 0;

    preview = (len > 60) ? 60 : len;
    for (i = 0; i < preview; i++)           /* keep the preview on one line */
        preview_buf[i] = (isspace((unsigned char)buf[i])) ? ' ' : buf[i];
    preview_buf[preview] = '\0';
    printf("Loaded %zu characters of user text (%zu bytes allocated, "
           "capacity %zu).\n", len, cap, cap);
    printf("  starts with: \"%s%s\"\n",
           preview_buf, (len > preview) ? "..." : "");
}

static void count_words(TextAnalyzer *a)
{
    if (!require_text(a))
        return;
    ensure_stats(a);
    printf("Total words     : %zu\n", a->total_words);
    printf("Distinct words  : %zu\n", a->distinct_count);
    printf("Truncated words : %zu (longer than %d characters)\n",
           a->truncated_words, WORD_MAX);
}

static void longest_word(TextAnalyzer *a)
{
    const char *w;
    size_t      len;

    if (!require_text(a))
        return;
    w = find_longest(a, &len);
    if (w == NULL) {
        printf("No words found in the text.\n");
        return;
    }
    printf("Longest word: \"%.*s\"\n", (int)len, w);
    printf("True length : %zu characters (measured on the raw text)\n", len);
    if (len > (size_t)WORD_MAX)
        printf("Note        : stored truncated to %d characters in the "
               "statistics table (truncated_words counts it).\n", WORD_MAX);
}

static void most_frequent(TextAnalyzer *a)
{
    size_t best;

    if (!require_text(a))
        return;
    ensure_stats(a);
    if (a->distinct_count == 0) {
        printf("No words to rank.\n");
        return;
    }
    best = most_frequent_index(a);
    printf("Most frequent word: \"%s\"\n", a->stats[best].name);
    printf("Occurrences       : %d (%.2f%% of %zu words)\n",
           a->stats[best].count,
           (a->total_words > 0)
               ? (double)a->stats[best].count / (double)a->total_words * 100.0
               : 0.0,
           a->total_words);
}

static void display_word_statistics(TextAnalyzer *a)
{
    size_t i;

    if (!require_text(a))
        return;
    ensure_stats(a);
    if (a->distinct_count == 0) {
        printf("No distinct words to display.\n");
        return;
    }
    printf("    %-40s %8s\n", "Word", "Count");
    printf("    %-40s %8s\n",
           "----------------------------------------", "--------");
    for (i = 0; i < a->distinct_count; i++)
        printf("    %-40s %8d\n", a->stats[i].name, a->stats[i].count);
    printf("\n    Totals: %zu words, %zu distinct, %zu truncated "
           "to %d characters.\n",
           a->total_words, a->distinct_count, a->truncated_words, WORD_MAX);
}

static void sort_word_statistics(TextAnalyzer *a)
{
    char   line[LINE_BUF];
    long   choice;
    size_t i, show;

    if (!require_text(a))
        return;
    ensure_stats(a);
    if (a->distinct_count < 2) {
        printf("Nothing to sort (%zu distinct word(s)).\n",
               a->distinct_count);
        return;
    }

    for (;;) {
        printf("Sort word statistics by:\n");
        printf("  1. Alphabetical (strcmp, A-Z)\n");
        printf("  2. Frequency (high to low, ties alphabetical)\n");
        printf("  0. Cancel\n");
        printf("Your choice: ");
        fflush(stdout);
        if (!read_line(line, sizeof line)) {
            printf("\nSort cancelled (end of input).\n");
            return;
        }
        if (!parse_int(line, &choice)) {
            printf("Invalid choice, enter 0, 1 or 2.\n");
            continue;
        }
        if (choice == 0) {
            printf("Sort cancelled.\n");
            return;
        }
        if (choice == 1 || choice == 2)
            break;
        printf("Invalid choice, enter 0, 1 or 2.\n");
    }

    if (choice == 1) {
        qsort(a->stats, a->distinct_count, sizeof *a->stats, cmp_alpha);
        printf("Sorted alphabetically with qsort(cmp_alpha).\n");
    } else {
        qsort(a->stats, a->distinct_count, sizeof *a->stats, cmp_freq);
        printf("Sorted by frequency with qsort(cmp_freq): "
               "count descending, ties alphabetical.\n");
    }
    show = (a->distinct_count < 5) ? a->distinct_count : 5;
    printf("First %zu entries:\n", show);
    for (i = 0; i < show; i++)
        printf("    %-40s %8d\n", a->stats[i].name, a->stats[i].count);
    printf("(order is kept until the text changes; option 6 shows the "
           "full table)\n");
}

static void search_word(TextAnalyzer *a)
{
    char        line[LINE_BUF];
    char        key[WORD_MAX + 1];
    const char *start, *hit;
    size_t      len, i, n;

    if (!require_text(a))
        return;
    printf("Word to search: ");
    fflush(stdout);
    if (!read_line(line, sizeof line)) {
        printf("\nSearch cancelled (end of input).\n");
        return;
    }
    hit = next_word(line, &start, &len);    /* same rules as the parser */
    if (hit == NULL) {
        printf("No word in that input - nothing to search for.\n");
        return;
    }
    n = (len > (size_t)WORD_MAX) ? (size_t)WORD_MAX : len;
    for (i = 0; i < n; i++)
        key[i] = (char)tolower((unsigned char)start[i]);
    key[n] = '\0';

    ensure_stats(a);
    a->has_search = 1;
    strcpy(a->last_search, key);            /* strlen(key) <= 39 */

    for (i = 0; i < a->distinct_count; i++) {
        if (strcmp(a->stats[i].name, key) == 0) {
            printf("Found \"%s\": %d occurrence(s) "
                   "(normalised search input).\n",
                   key, a->stats[i].count);
            return;
        }
    }
    printf("\"%s\" not found in the text.\n", key);
}

static void generate_text_summary(TextAnalyzer *a)
{
    Metrics     m;
    const char *w;
    size_t      wlen;

    print_header(a->source_is_user);
    if (!require_text(a))
        return;
    ensure_stats(a);
    compute_metrics(a, &m);
    w = find_longest(a, &wlen);

    printf("  %-24s: %zu\n", "Total words", a->total_words);
    printf("  %-24s: %zu\n", "Distinct words", a->distinct_count);
    printf("  %-24s: %.2f characters\n", "Average word length", m.avg_len);
    if (w != NULL)
        printf("  %-24s: \"%.*s\" (%zu characters)\n",
               "Longest word", (int)wlen, w, wlen);
    else
        printf("  %-24s: (none)\n", "Longest word");
    if (a->distinct_count > 0)
        printf("  %-24s: \"%s\" (%d occurrences)\n", "Most frequent word",
               a->stats[most_frequent_index(a)].name,
               a->stats[most_frequent_index(a)].count);
    else
        printf("  %-24s: (none)\n", "Most frequent word");
    printf("  %-24s: %zu\n", "Truncated words", a->truncated_words);
    printf("\n  Custom analysis - Lexical Diversity Score\n");
    printf("  %-24s: %.2f%% (%s)\n", "LDS", m.lds, lds_band(m.lds));
    printf("  %-24s: %.2f%%\n", "Repetition intensity", m.repetition);
    printf("  %-24s: %.2f%%\n", "Hapax ratio", m.hapax);
    printf("\n  Memory inspection (same as menu 11)\n");
    print_memory_details(a);
}

static void lexical_diversity_analysis(TextAnalyzer *a)
{
    Metrics     m;
    size_t      i, show;
    struct wordStat *top;

    if (!require_text(a))
        return;
    ensure_stats(a);
    compute_metrics(a, &m);

    printf("------------------------------------------------------------------\n");
    printf(" CUSTOM ANALYSIS: Lexical Diversity Score (LDS)\n");
    printf("------------------------------------------------------------------\n");
    printf("Purpose\n");
    printf("  The LDS is the type-token ratio expressed as a percentage. "
           "It\n"
           "  measures how varied the vocabulary is: the more the text "
           "reuses\n"
           "  the same words, the lower the score.  Together with the "
           "supporting\n"
           "  metrics below it separates \"many different words\" from "
           "\"the same\n"
           "  few words repeated\".\n\n");

    if (a->total_words == 0 || a->distinct_count == 0) {
        printf("No words available - load text and run option 3 first.\n");
        return;
    }

    printf("Formulas and current values\n");
    printf("  LDS                  = distinct_words / total_words * 100\n");
    printf("                       = %zu / %zu * 100 = %.2f%%\n",
           a->distinct_count, a->total_words, m.lds);
    printf("  Repetition Intensity = distinct words occurring 2 or more "
           "times\n"
           "                       / distinct_words * 100\n");
    printf("                       = %zu / %zu * 100 = %.2f%%\n",
           m.repeated_count, a->distinct_count, m.repetition);
    printf("  Hapax ratio          = distinct words occurring exactly once\n"
           "                       / distinct_words * 100\n");
    printf("                       = %zu / %zu * 100 = %.2f%%\n",
           m.hapax_count, a->distinct_count, m.hapax);
    printf("  Average word length  = total letters in all words / "
           "total_words\n");
    printf("                       = %zu / %zu = %.2f characters\n\n",
           a->total_letters, a->total_words, m.avg_len);

    printf("Interpretation guide\n");
    printf("  Heuristic thresholds for short passages: the score falls as "
           "texts\n"
           "  get longer, because long texts repeat more vocabulary.\n");
    printf("    at least 75      : very varied vocabulary\n");
    printf("    55 to 74.99      : moderately varied\n");
    printf("    below 55         : repetitive\n");
    printf("  Current text: %.2f%% -> %s\n\n", m.lds, lds_band(m.lds));

    printf("Top 5 most repeated words\n");
    top = malloc(a->distinct_count * sizeof *top);
    if (top == NULL) {
        fprintf(stderr, "Error: out of memory building the top-5 list.\n");
        return;
    }
    memcpy(top, a->stats, a->distinct_count * sizeof *top);
    qsort(top, a->distinct_count, sizeof *top, cmp_freq);
    show = (a->distinct_count < 5) ? a->distinct_count : 5;
    for (i = 0; i < show; i++)
        printf("  %zu. %-24s %d\n", i + 1, top[i].name, top[i].count);
    free(top);
}

static void memory_inspection(TextAnalyzer *a)
{
    printf("------------------------------------------------------------------\n");
    printf(" MEMORY INSPECTION (addresses printed with %%p as (void *))\n");
    printf("------------------------------------------------------------------\n");
    print_memory_details(a);
    printf("  How to read it: each address is the start of a block obtained\n"
           "  from malloc/realloc on the heap; the selected entry is a "
           "member\n"
           "  of the array, so its address is the array base plus the byte\n"
           "  offset of that element.\n");
}

static void exit_program(TextAnalyzer *a)
{
    a->running = 0;
    printf("Exiting: all memory will now be freed.\n");
}

/* ---- menu --------------------------------------------------------------- */
static const Operation MENU_OPS[] = {
    exit_program,               /*  0 */
    use_default_text,           /*  1 */
    input_custom_text,          /*  2 */
    count_words,                /*  3 */
    longest_word,               /*  4 */
    most_frequent,              /*  5 */
    display_word_statistics,    /*  6 */
    sort_word_statistics,       /*  7 */
    search_word,                /*  8 */
    generate_text_summary,      /*  9 */
    lexical_diversity_analysis, /* 10 */
    memory_inspection           /* 11 */
};

static const char *const MENU_LABELS[] = {
    "  0. Exit",
    "  1. Use the default Austen paragraph",
    "  2. Enter or paste my own paragraph",
    "  3. Count words",
    "  4. Longest word",
    "  5. Most frequent word",
    "  6. Display word statistics",
    "  7. Sort word statistics",
    "  8. Search for a word",
    "  9. Text summary",
    " 10. Lexical Diversity analysis (custom)",
    " 11. Memory inspection"
};

static void print_menu(void)
{
    size_t i;

    printf("\n---------------------------- MENU ----------------------------\n");
    for (i = 1; i < sizeof MENU_LABELS / sizeof MENU_LABELS[0]; i++)
        printf("%s\n", MENU_LABELS[i]);
    printf("%s\n", MENU_LABELS[0]);
    printf("--------------------------------------------------------------\n");
}

/* Returns 0 on EOF, otherwise *out is the choice, or -1 when invalid. */
static int read_menu_choice(int *out)
{
    char line[LINE_BUF];
    long v;

    if (!read_line(line, sizeof line))
        return 0;
    if (!parse_int(line, &v) || v < 0 || v > 11) {
        *out = -1;
        return 1;
    }
    *out = (int)v;
    return 1;
}

static void cleanup(TextAnalyzer *a)
{
    printf("Freed: text buffer (%s, %zu bytes) and word statistics array "
           "(%s, %zu entries).\n",
           (a->text != NULL) ? "heap block" : "none",
           (a->text != NULL) ? a->text_cap : (size_t)0,
           (a->stats != NULL) ? "heap block" : "none",
           (a->stats != NULL) ? a->stats_cap : (size_t)0);
    free(a->text);
    a->text = NULL;
    a->text_len = 0;
    a->text_cap = 0;
    free(a->stats);
    a->stats = NULL;
    a->distinct_count = 0;
    a->stats_cap = 0;
}

int main(void)
{
    TextAnalyzer a;

    memset(&a, 0, sizeof a);
    a.stats_dirty = 1;
    a.running = 1;

    print_header(a.source_is_user);

    while (a.running) {
        int choice = -1;

        print_menu();
        printf("Choose an option (0-11): ");
        fflush(stdout);
        if (!read_menu_choice(&choice)) {
            printf("\nEnd of input detected - exiting.\n");
            break;
        }
        if (choice < 0) {
            printf("Invalid option. Please enter a whole number "
                   "from 0 to 11.\n");
            continue;
        }
        MENU_OPS[choice](&a);               /* dispatch, no switch */
    }

    cleanup(&a);
    return 0;
}
