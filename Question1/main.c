/*
 * ==========================================================================
 *  Dynamic Academic Performance and Risk Analysis Engine
 *  Custom Analysis: Academic Risk Index
 *
 *  Build:  gcc -Wall -Wextra -std=c11 -o engine main.c
 *  Debug:  gcc -Wall -Wextra -std=c11 -g -fsanitize=address,undefined \
 *              -o engine_asan main.c
 *
 *  Design notes
 *    - Records live in a dynamically allocated array (realloc growth and
 *      shrink) held inside a StudentDB struct that is passed by pointer;
 *      there is no global mutable state.
 *    - The menu is dispatched through an array of function pointers, and
 *      qsort() is driven by comparator function pointers.
 *    - All input comes from fgets() with an explicit size limit.
 * ==========================================================================
 */

#define _POSIX_C_SOURCE 200809L   /* isatty()/fileno() under -std=c11 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* ----------------------------- constants -------------------------------- */

#define STUDENT_ID          "12345"                    /* single place to edit */
#define PROJECT_NAME        "Dynamic Academic Performance and Risk Analysis Engine"
#define CUSTOM_ANALYSIS     "Academic Risk Index"

#define NAME_LEN            64                         /* id/name buffers      */
#define LINE_LEN            256                        /* input line limit     */
#define INITIAL_CAPACITY    4                          /* first array size     */

#define MIN_GRADE           0.0
#define MAX_GRADE           100.0
#define PASS_MARK           50.0                       /* summary report only  */
#define DEFAULT_RISK_THRESHOLD 60.0                    /* Academic Risk Index  */

#define MIN_THRESHOLD       1.0
#define MAX_THRESHOLD       100.0

/* status codes returned by prompt_line() */
enum { READ_OK = 1, READ_EOF = 0, READ_TOO_LONG = -1 };

/* ------------------------------ data model ------------------------------ */

typedef struct {
    int    id;
    char   name[NAME_LEN];
    char   course[NAME_LEN];
    double grade;                 /* 0 .. 100 */
} Student;

typedef struct {
    Student *students;            /* dynamically allocated array */
    size_t   count;               /* records in use               */
    size_t   capacity;            /* records that fit             */
    int      running;             /* cleared by the Exit action   */
} StudentDB;

/* one row of the Academic Risk Index table */
typedef struct {
    Student student;              /* copy, so the table can be sorted */
    double  risk;                 /* 0 .. 100 */
} RiskEntry;

/* aggregate results shared by the analysis and the summary report */
typedef struct {
    size_t safe;
    size_t low;
    size_t moderate;
    size_t high;
    double average;               /* class-wide average risk index */
} RiskStats;

typedef void (*MenuAction)(StudentDB *db);

/* ------------------------- small string helpers ------------------------- */

/* Removes surrounding whitespace in place so the buffer can be re-read. */
static void trim_inplace(char *buf)
{
    char *start = buf;
    size_t len;

    while (*start != '\0' && isspace((unsigned char)*start))
        start++;
    if (start != buf)
        memmove(buf, start, strlen(start) + 1);

    len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len - 1]))
        buf[--len] = '\0';
}

/* Portable case-insensitive comparison (used for searching by name). */
static int str_case_cmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* Case-insensitive substring search: portable replacement for strcasestr().
   Returns a pointer to the first match, or NULL. */
static const char *str_case_find(const char *haystack, const char *needle)
{
    if (*needle == '\0')
        return haystack;
    for (const char *h = haystack; *h != '\0'; h++) {
        if (str_case_cmp(h, needle) == 0)
            return h;
    }
    return NULL;
}

/* --------------------------- numeric parsing ---------------------------- */

/* Full-string integer parse; returns 0 when the text is not a plain integer. */
static int parse_int(const char *text, int *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (end == text || end == NULL || *end != '\0')
        return 0;
    if (errno == ERANGE || value > INT_MAX || value < INT_MIN)
        return 0;
    *out = (int)value;
    return 1;
}

/* Full-string double parse; rejects "nan"/"inf" as well as junk. */
static int parse_double(const char *text, double *out)
{
    char *end = NULL;
    double value;

    errno = 0;
    value = strtod(text, &end);
    if (end == text || end == NULL || *end != '\0')
        return 0;
    if (errno == ERANGE || value != value)   /* overflow or NaN */
        return 0;
    *out = value;
    return 1;
}

/* ----------------------------- input layer ------------------------------ */

/*
 * Piped input is not echoed by a terminal, so during scripted runs we echo
 * the value ourselves to keep the captured log readable.  On a real terminal
 * the terminal already echoes it, so we stay quiet.
 */
static int stdin_is_interactive(void)
{
#if defined(_WIN32)
    return 1;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

/*
 * Prints a prompt, reads one line with fgets() into buf (size limited),
 * strips the newline, flushes any overflow past the buffer, trims the
 * surrounding whitespace and returns READ_OK / READ_EOF / READ_TOO_LONG.
 */
static int prompt_line(const char *prompt, char *buf, size_t size)
{
    int too_long = 0;
    size_t len;

    printf("%s", prompt);
    fflush(stdout);

    if (fgets(buf, (int)size, stdin) == NULL) {
        printf("\n");
        return READ_EOF;
    }

    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    } else {
        /* No newline: either end of file or a line longer than the buffer.
           Discard the rest of the line so the next prompt stays aligned. */
        int c;
        while ((c = getchar()) != EOF && c != '\n')
            too_long = 1;
    }

    trim_inplace(buf);

    if (too_long) {
        printf("\n");
        return READ_TOO_LONG;
    }

    if (!stdin_is_interactive())
        printf("%s\n", buf);        /* reproduce the value that was piped in */

    return READ_OK;
}

/* ------------------------------ header ---------------------------------- */

static void print_header(void)
{
    printf("================================================================\n");
    printf("Student ID: %s\n", STUDENT_ID);
    printf("Project: %s\n", PROJECT_NAME);
    printf("Custom Analysis: %s\n", CUSTOM_ANALYSIS);
    printf("================================================================\n");
}

static void print_menu(void)
{
    printf("\n---------------------------- MENU ----------------------------\n");
    printf("  1. Add student\n");
    printf("  2. Display all students\n");
    printf("  3. Sort students\n");
    printf("  4. Calculate average grade\n");
    printf("  5. Find top-performing student\n");
    printf("  6. Search student\n");
    printf("  7. Remove student (by ID)\n");
    printf("  8. Run custom analysis (Academic Risk Index)\n");
    printf("  9. Display summary report\n");
    printf(" 10. Exit\n");
    printf("--------------------------------------------------------------\n");
}

/* --------------------------- database layer ----------------------------- */

static int db_init(StudentDB *db)
{
    db->students = calloc(INITIAL_CAPACITY, sizeof *db->students);
    if (db->students == NULL) {
        fprintf(stderr, "Fatal error: could not allocate the student database.\n");
        return 0;
    }
    db->count    = 0;
    db->capacity = INITIAL_CAPACITY;
    db->running  = 1;
    return 1;
}

/* Frees everything; called on every exit path from main(). */
static void db_free(StudentDB *db)
{
    free(db->students);             /* free(NULL) is safe after a failed init */
    db->students = NULL;
    db->count    = 0;
    db->capacity = 0;
}

/* Grows the array (doubling) with a temporary pointer so a failed realloc
   can never lose the original block. */
static int db_ensure_capacity(StudentDB *db, size_t needed)
{
    size_t newcap;
    Student *tmp;

    if (needed <= db->capacity)
        return 1;

    newcap = db->capacity > 0 ? db->capacity : INITIAL_CAPACITY;
    while (newcap < needed)
        newcap *= 2;

    tmp = realloc(db->students, newcap * sizeof *db->students);
    if (tmp == NULL) {
        fprintf(stderr, "Error: out of memory (cannot grow to %zu records); "
                        "the existing data is untouched.\n", newcap);
        return 0;
    }
    db->students = tmp;
    db->capacity = newcap;
    printf("[memory] array grew:   count=%zu capacity=%zu\n",
           db->count, db->capacity);
    return 1;
}

/* Shrinks the array once it is less than one quarter full, again through a
   temporary pointer, and never below INITIAL_CAPACITY or below count. */
static void db_shrink_if_needed(StudentDB *db)
{
    size_t newcap;
    Student *tmp;

    if (db->capacity <= INITIAL_CAPACITY)
        return;
    if (db->count >= db->capacity / 4)
        return;

    newcap = db->capacity / 2;
    if (newcap < INITIAL_CAPACITY)
        newcap = INITIAL_CAPACITY;
    if (newcap < db->count)
        newcap = db->count;
    if (newcap >= db->capacity)
        return;

    tmp = realloc(db->students, newcap * sizeof *db->students);
    if (tmp == NULL) {
        fprintf(stderr, "Warning: could not shrink the array; "
                        "keeping capacity %zu.\n", db->capacity);
        return;
    }
    db->students = tmp;
    db->capacity = newcap;
    printf("[memory] array shrank: count=%zu capacity=%zu\n",
           db->count, db->capacity);
}

static int db_append(StudentDB *db, const Student *src)
{
    if (!db_ensure_capacity(db, db->count + 1))
        return 0;
    db->students[db->count] = *src;
    db->count++;
    return 1;
}

static Student *db_find_by_id(StudentDB *db, int id)
{
    Student *end = db->students + db->count;

    for (Student *p = db->students; p < end; p++) {
        if (p->id == id)
            return p;
    }
    return NULL;
}

/* --------------------------- seed dataset ------------------------------- */

/* 12 records: unique IDs, three courses, grades 20..98 so that every risk
   band (Safe / Low / Moderate / High) is represented at least once. */
static const Student SEED_DATA[] = {
    { 1001, "Alice Johnson",   "Mathematics",       98.0 },
    { 1002, "Brian Okafor",    "Physics",           74.0 },
    { 1003, "Chloe Ramirez",   "Programming in C",  88.0 },
    { 1004, "Daniel Petrov",   "Mathematics",       61.0 },
    { 1005, "Emma Thompson",   "Physics",           58.0 },
    { 1006, "Farid Haddad",    "Programming in C",  45.0 },
    { 1007, "Grace Kim",       "Mathematics",       42.0 },
    { 1008, "Hugo Silva",      "Physics",           37.0 },
    { 1009, "Isabel Novak",    "Programming in C",  30.0 },
    { 1010, "Jamal Wright",    "Mathematics",       27.0 },
    { 1011, "Karla Mendes",    "Physics",           20.0 },
    { 1012, "Liam O'Brien",    "Programming in C",  55.0 }
};

static int seed_database(StudentDB *db)
{
    size_t n = sizeof SEED_DATA / sizeof SEED_DATA[0];

    for (size_t i = 0; i < n; i++) {
        if (!db_append(db, &SEED_DATA[i])) {
            db_free(db);
            return 0;
        }
    }
    return 1;
}

/* ------------------------ Academic Risk Index --------------------------- */

/* 0 when the grade is at or above the safe level, otherwise the percentage
   distance below it: (threshold - grade) / threshold * 100. */
static double risk_index(double grade, double threshold)
{
    if (grade >= threshold)
        return 0.0;
    return (threshold - grade) / threshold * 100.0;
}

/* Bands: 0 = Safe; (0,25] = Low; (25,50] = Moderate; >50 = High. */
static const char *risk_band(double risk)
{
    if (risk <= 0.0)
        return "Safe";
    if (risk <= 25.0)
        return "Low risk";
    if (risk <= 50.0)
        return "Moderate risk";
    return "High risk";
}

static void compute_risk_stats(const StudentDB *db, double threshold,
                               RiskStats *out)
{
    double total = 0.0;

    out->safe = out->low = out->moderate = out->high = 0;
    out->average = 0.0;
    if (db->count == 0)
        return;

    for (const Student *p = db->students;
         p < db->students + db->count; p++) {
        double r = risk_index(p->grade, threshold);
        total += r;
        if (r <= 0.0)
            out->safe++;
        else if (r <= 25.0)
            out->low++;
        else if (r <= 50.0)
            out->moderate++;
        else
            out->high++;
    }
    out->average = total / (double)db->count;
}

/* Newest risk first; ties broken by name so the table is stable. */
static int compare_risk_desc(const void *a, const void *b)
{
    const RiskEntry *ra = (const RiskEntry *)a;
    const RiskEntry *rb = (const RiskEntry *)b;

    if (ra->risk < rb->risk)
        return 1;
    if (ra->risk > rb->risk)
        return -1;
    return strcmp(ra->student.name, rb->student.name);
}

static void print_risk_stats(const RiskStats *stats, double threshold,
                             size_t total)
{
    printf("  Band counts over %zu student(s) (threshold %.0f):\n", total,
           threshold);
    printf("    Safe          : %zu\n", stats->safe);
    printf("    Low risk      : %zu\n", stats->low);
    printf("    Moderate risk : %zu\n", stats->moderate);
    printf("    High risk     : %zu\n", stats->high);
    printf("  Class-wide average risk index : %.2f / 100\n", stats->average);
}

static void print_risk_explanation(double threshold)
{
    printf("\n  --- How to read the Academic Risk Index ---\n");
    printf(
        "  Purpose : an early-warning score showing how far a grade sits below\n"
        "            a safe level, so a 58 and a 20 are not treated as equally\n"
        "            at risk: the further below the threshold, the higher the\n"
        "            score.\n"
        "  Formula : risk = 0                                     if grade >= threshold\n"
        "            risk = (threshold - grade) / threshold * 100   otherwise\n"
        "  Worked example (default threshold 60):\n"
        "              grade 55 -> (60-55)/60*100 =  8.3\n"
        "              grade 45 -> (60-45)/60*100 = 25.0\n"
        "              grade 30 -> (60-30)/60*100 = 50.0\n"
        "              grade  0 -> (60- 0)/60*100 = 100.0\n"
        "  Bands   : 0 = Safe; above 0 up to 25 = Low risk; above 25 up to 50 =\n"
        "            Moderate risk; above 50 = High risk.\n"
        "  Reading : 0 means the student is at or above the safe level.  The\n"
        "            higher the index the further below it; 100 means a grade\n"
        "            of 0.  Only students above 0 are listed as at risk.\n"
        "  Current threshold in use: %.0f, so the same formula gives, for example,\n"
        "              grade %.1f (half the threshold) -> 50.0   and   grade 0 -> 100.0\n"
        "  Note    : the pass mark (%.0f) is a separate scale used by the summary\n"
        "            report; this analysis always uses the threshold shown here.\n",
        threshold, threshold / 2.0, PASS_MARK);
}

/* The required custom analysis entry point. */
static void academic_risk_analysis(StudentDB *db)
{
    char buf[LINE_LEN];
    double threshold = DEFAULT_RISK_THRESHOLD;
    RiskStats stats;
    size_t at_risk = 0;
    RiskEntry *entries = NULL;
    int rc;

    if (db->count == 0) {
        printf("The database is empty: there are no students to analyse.\n");
        return;
    }

    rc = prompt_line("Risk threshold 1-100 [Enter = default 60]: ",
                     buf, sizeof buf);
    if (rc == READ_EOF) {
        printf("Input ended before a threshold was given; analysis cancelled.\n");
        return;
    }
    if (rc == READ_TOO_LONG) {
        printf("Input too long; analysis cancelled.\n");
        return;
    }
    if (buf[0] != '\0') {                       /* blank = default */
        double value;
        if (!parse_double(buf, &value) || value < MIN_THRESHOLD ||
            value > MAX_THRESHOLD) {
            printf("Invalid threshold '%s': must be a number from 1 to 100. "
                   "Analysis cancelled.\n", buf);
            return;
        }
        threshold = value;
    }

    printf("\n==================== %s ====================\n", CUSTOM_ANALYSIS);
    printf("Risk threshold: %.0f  (pass mark used by the report: %.0f)\n\n",
           threshold, PASS_MARK);

    for (const Student *p = db->students;
         p < db->students + db->count; p++) {
        if (risk_index(p->grade, threshold) > 0.0)
            at_risk++;
    }

    if (at_risk == 0) {
        printf("No at-risk students: every grade is at or above the threshold.\n");
    } else {
        entries = malloc(at_risk * sizeof *entries);
        if (entries == NULL) {
            fprintf(stderr, "Error: out of memory; cannot build the "
                            "at-risk table.\n");
        } else {
            size_t n = 0;
            for (const Student *p = db->students;
                 p < db->students + db->count; p++) {
                double r = risk_index(p->grade, threshold);
                if (r > 0.0) {
                    entries[n].student = *p;
                    entries[n].risk = r;
                    n++;
                }
            }
            if (n > 1)
                qsort(entries, n, sizeof *entries, compare_risk_desc);

            printf("At-risk students (%zu of %zu), highest risk first:\n",
                   n, db->count);
            printf("%6s  %-20s %-22s %6s  %5s  %s\n", "ID", "Name", "Course",
                   "Grade", "Risk", "Band");
            printf("------  -------------------- ---------------------- "
                   "------  -----  -------------\n");
            for (RiskEntry *e = entries; e < entries + n; e++) {
                printf("%6d  %-20s %-22s %6.1f  %5.1f  %s\n",
                       e->student.id, e->student.name, e->student.course,
                       e->student.grade, e->risk, risk_band(e->risk));
            }
            printf("\n");
            free(entries);
            entries = NULL;
        }
    }

    compute_risk_stats(db, threshold, &stats);
    print_risk_stats(&stats, threshold, db->count);
    print_risk_explanation(threshold);
}

/* ---------------------------- sort support ------------------------------ */

static int compare_by_grade(const void *a, const void *b)
{
    const Student *sa = (const Student *)a;
    const Student *sb = (const Student *)b;

    if (sa->grade < sb->grade)
        return -1;
    if (sa->grade > sb->grade)
        return 1;
    return 0;
}

static int compare_by_name(const void *a, const void *b)
{
    const Student *sa = (const Student *)a;
    const Student *sb = (const Student *)b;

    return strcmp(sa->name, sb->name);          /* case-sensitive ordering */
}

static int compare_by_id(const void *a, const void *b)
{
    const Student *sa = (const Student *)a;
    const Student *sb = (const Student *)b;

    if (sa->id < sb->id)
        return -1;
    if (sa->id > sb->id)
        return 1;
    return 0;
}

/* Descending order = sort ascending, then swap the ends inwards. */
static void reverse_students(StudentDB *db)
{
    Student *lo = db->students;
    Student *hi = db->students + db->count - 1;

    if (db->count < 2)
        return;
    while (lo < hi) {
        Student tmp = *lo;
        *lo = *hi;
        *hi = tmp;
        lo++;
        hi--;
    }
}

/* --------------------------- shared display ----------------------------- */

static void print_table_head(void)
{
    printf("%6s  %-20s %-22s %6s\n", "ID", "Name", "Course", "Grade");
    printf("------  -------------------- ---------------------- "
           "------\n");
}

/* ============================= MENU ACTIONS ============================= */

static void action_add(StudentDB *db)
{
    char id_buf[LINE_LEN];
    char name_buf[LINE_LEN];
    char course_buf[LINE_LEN];
    char grade_buf[LINE_LEN];
    Student s;
    int errors = 0;
    double grade;
    int rc;

    memset(&s, 0, sizeof s);
    memset(id_buf, 0, sizeof id_buf);
    memset(name_buf, 0, sizeof name_buf);
    memset(course_buf, 0, sizeof course_buf);
    memset(grade_buf, 0, sizeof grade_buf);

    /* Read all four fields first so piped input stays aligned, then validate. */
    rc = prompt_line("Student ID (positive integer): ", id_buf, sizeof id_buf);
    if (rc != READ_OK)
        goto input_problem;
    rc = prompt_line("Student name: ", name_buf, sizeof name_buf);
    if (rc != READ_OK)
        goto input_problem;
    rc = prompt_line("Course: ", course_buf, sizeof course_buf);
    if (rc != READ_OK)
        goto input_problem;
    rc = prompt_line("Grade (0-100): ", grade_buf, sizeof grade_buf);
    if (rc != READ_OK)
        goto input_problem;

    if (!parse_int(id_buf, &s.id) || s.id <= 0) {
        printf("Rejected: invalid ID '%s' (must be a positive integer).\n",
               id_buf);
        errors++;
    } else if (db_find_by_id(db, s.id) != NULL) {
        printf("Rejected: duplicate ID %d - a student with this ID already "
               "exists.\n", s.id);
        errors++;
    }

    if (name_buf[0] == '\0') {
        printf("Rejected: name must not be empty.\n");
        errors++;
    } else if (strlen(name_buf) >= NAME_LEN) {
        printf("Rejected: name is too long (max %d characters).\n",
               NAME_LEN - 1);
        errors++;
    } else {
        strncpy(s.name, name_buf, NAME_LEN - 1);
        s.name[NAME_LEN - 1] = '\0';
    }

    if (course_buf[0] == '\0') {
        printf("Rejected: course must not be empty.\n");
        errors++;
    } else if (strlen(course_buf) >= NAME_LEN) {
        printf("Rejected: course is too long (max %d characters).\n",
               NAME_LEN - 1);
        errors++;
    } else {
        strncpy(s.course, course_buf, NAME_LEN - 1);
        s.course[NAME_LEN - 1] = '\0';
    }

    if (!parse_double(grade_buf, &grade)) {
        printf("Rejected: grade '%s' is not a number "
               "(must be between 0 and 100).\n", grade_buf);
        errors++;
    } else if (grade < MIN_GRADE || grade > MAX_GRADE) {
        printf("Rejected: grade %.2f is out of range (must be 0 to 100).\n",
               grade);
        errors++;
    } else {
        s.grade = grade;
    }

    if (errors > 0) {
        printf("Record not added (%d problem%s).\n", errors,
               errors == 1 ? "" : "s");
        return;
    }

    if (!db_append(db, &s)) {
        fprintf(stderr, "Record not added: memory allocation failed.\n");
        return;
    }
    printf("Added '%s' (ID %d, %s, %.1f). Records: %zu, capacity: %zu\n",
           s.name, s.id, s.course, s.grade, db->count, db->capacity);
    return;

input_problem:
    if (rc == READ_EOF)
        printf("Input ended; add cancelled.\n");
    else
        printf("Input too long; add cancelled.\n");
}

static void action_display(StudentDB *db)
{
    if (db->count == 0) {
        printf("No records to display: the database is empty.\n");
        return;
    }

    print_table_head();
    for (const Student *p = db->students;
         p < db->students + db->count; p++) {
        printf("%6d  %-20s %-22s %6.1f\n", p->id, p->name, p->course,
               p->grade);
    }
    printf("Total records: %zu (capacity %zu)\n", db->count, db->capacity);
}

static void action_sort(StudentDB *db)
{
    char buf[LINE_LEN];
    int by, dir, rc;
    const char *field_name;
    const char *dir_name;
    int (*comparator)(const void *, const void *);

    if (db->count == 0) {
        printf("Nothing to sort: the database is empty.\n");
        return;
    }

    printf("  Sort by: 1) grade   2) name   3) ID\n");
    rc = prompt_line("Your choice: ", buf, sizeof buf);
    if (rc != READ_OK) {
        printf("Sort cancelled.\n");
        return;
    }
    if (!parse_int(buf, &by) || by < 1 || by > 3) {
        printf("Invalid sort field '%s'; sort cancelled.\n", buf);
        return;
    }

    printf("  Order: 1) ascending   2) descending\n");
    rc = prompt_line("Your choice: ", buf, sizeof buf);
    if (rc != READ_OK) {
        printf("Sort cancelled.\n");
        return;
    }
    if (!parse_int(buf, &dir) || dir < 1 || dir > 2) {
        printf("Invalid sort order '%s'; sort cancelled.\n", buf);
        return;
    }

    switch (by) {
    case 1:
        comparator = compare_by_grade;
        field_name = "grade";
        break;
    case 2:
        comparator = compare_by_name;
        field_name = "name";
        break;
    default:
        comparator = compare_by_id;
        field_name = "ID";
        break;
    }

    qsort(db->students, db->count, sizeof *db->students, comparator);
    if (dir == 2)
        reverse_students(db);

    dir_name = (dir == 1) ? "ascending" : "descending";
    printf("Sorted by %s (%s). %zu record(s) reordered.\n",
           field_name, dir_name, db->count);
}

static void action_average(StudentDB *db)
{
    double sum = 0.0;
    const Student *end = db->students + db->count;

    if (db->count == 0) {
        printf("Cannot calculate an average: the database is empty.\n");
        return;
    }

    for (const Student *p = db->students; p < end; p++)
        sum += p->grade;

    printf("Average grade over %zu student(s): %.2f\n",
           db->count, sum / (double)db->count);
}

static void action_find_top(StudentDB *db)
{
    const Student *top;
    const Student *end = db->students + db->count;

    if (db->count == 0) {
        printf("No top performer: the database is empty.\n");
        return;
    }

    top = db->students;
    for (const Student *p = db->students + 1; p < end; p++) {
        if (p->grade > top->grade)
            top = p;
    }

    printf("Top-performing student: %s (ID %d, %s) with %.1f\n",
           top->name, top->id, top->course, top->grade);
}

static void action_search(StudentDB *db)
{
    char buf[LINE_LEN];
    int choice, rc;

    if (db->count == 0) {
        printf("Nothing to search: the database is empty.\n");
        return;
    }

    printf("  Search by: 1) ID   2) name\n");
    rc = prompt_line("Your choice: ", buf, sizeof buf);
    if (rc != READ_OK) {
        printf("Search cancelled.\n");
        return;
    }
    if (!parse_int(buf, &choice) || choice < 1 || choice > 2) {
        printf("Invalid search option '%s'; search cancelled.\n", buf);
        return;
    }

    if (choice == 1) {
        int id;
        Student *found;

        rc = prompt_line("Student ID to find: ", buf, sizeof buf);
        if (rc != READ_OK) {
            printf("Search cancelled.\n");
            return;
        }
        if (!parse_int(buf, &id) || id <= 0) {
            printf("Invalid ID '%s': must be a positive integer.\n", buf);
            return;
        }
        found = db_find_by_id(db, id);
        if (found == NULL) {
            printf("Student not found: no record with ID %d.\n", id);
            return;
        }
        print_table_head();
        printf("%6d  %-20s %-22s %6.1f\n", found->id, found->name,
               found->course, found->grade);
    } else {
        size_t matches = 0;

        rc = prompt_line("Name to find (case-insensitive): ", buf, sizeof buf);
        if (rc != READ_OK) {
            printf("Search cancelled.\n");
            return;
        }
        if (buf[0] == '\0') {
            printf("Empty name: nothing to search for.\n");
            return;
        }
        print_table_head();
        for (const Student *p = db->students;
             p < db->students + db->count; p++) {
            if (str_case_find(p->name, buf) != NULL) {
                printf("%6d  %-20s %-22s %6.1f\n", p->id, p->name,
                       p->course, p->grade);
                matches++;
            }
        }
        if (matches == 0) {
            printf("Student not found: no record named '%s'.\n", buf);
        } else {
            printf("Matched %zu record(s).\n", matches);
        }
    }
}

static void action_remove(StudentDB *db)
{
    char buf[LINE_LEN];
    Student *found;
    size_t index;
    int id, rc;

    if (db->count == 0) {
        printf("Nothing to remove: the database is empty.\n");
        return;
    }

    rc = prompt_line("Student ID to remove: ", buf, sizeof buf);
    if (rc != READ_OK) {
        printf("Remove cancelled.\n");
        return;
    }
    if (!parse_int(buf, &id) || id <= 0) {
        printf("Invalid ID '%s': must be a positive integer.\n", buf);
        return;
    }

    found = db_find_by_id(db, id);
    if (found == NULL) {
        printf("Student not found: no record with ID %d, nothing removed.\n",
               id);
        return;
    }

    index = (size_t)(found - db->students);
    printf("Removed '%s' (ID %d).\n", found->name, found->id);
    memmove(found, found + 1,
            (db->count - index - 1) * sizeof *db->students);
    db->count--;
    db_shrink_if_needed(db);
    printf("Records: %zu, capacity: %zu\n", db->count, db->capacity);
}

static void action_risk(StudentDB *db)
{
    academic_risk_analysis(db);
}

static void action_summary(StudentDB *db)
{
    const Student *end;
    const Student *highest;
    const Student *lowest;
    double sum = 0.0;
    size_t at_or_above = 0;
    RiskStats stats;

    print_header();

    if (db->count == 0) {
        printf("No records: the database is empty, nothing to summarise.\n");
        return;
    }

    end = db->students + db->count;
    highest = db->students;
    lowest = db->students;
    for (const Student *p = db->students; p < end; p++) {
        sum += p->grade;
        if (p->grade > highest->grade)
            highest = p;
        if (p->grade < lowest->grade)
            lowest = p;
        if (p->grade >= PASS_MARK)
            at_or_above++;
    }

    printf("------------------------ SUMMARY REPORT ------------------------\n");
    printf("  Total records                 : %zu\n", db->count);
    printf("  Average grade                 : %.2f\n",
           sum / (double)db->count);
    printf("  Highest grade                 : %.1f - %s (ID %d, %s)\n",
           highest->grade, highest->name, highest->id, highest->course);
    printf("  Lowest grade                  : %.1f - %s (ID %d, %s)\n",
           lowest->grade, lowest->name, lowest->id, lowest->course);
    printf("  At or above pass mark (%3.0f)   : %zu\n", PASS_MARK, at_or_above);
    printf("  Below pass mark               : %zu\n",
           db->count - at_or_above);

    compute_risk_stats(db, DEFAULT_RISK_THRESHOLD, &stats);
    printf("  Academic Risk Index results (threshold %.0f):\n",
           DEFAULT_RISK_THRESHOLD);
    print_risk_stats(&stats, DEFAULT_RISK_THRESHOLD, db->count);
    printf("----------------------------------------------------------------\n");
}

static void action_exit(StudentDB *db)
{
    printf("Exiting: freeing %zu record(s). Goodbye!\n", db->count);
    db->running = 0;                 /* stops the menu loop in main() */
}

/* Hidden option 0 (not listed in the menu): shows the internal array state,
   used as evidence that growth and shrinking really happen. */
static void action_debug_stats(StudentDB *db)
{
    printf("[memory] internal state: count=%zu capacity=%zu (%zu bytes)\n",
           db->count, db->capacity,
           db->capacity * sizeof *db->students);
}

/* Menu index 0 is the hidden debug action; 1..10 are the visible options. */
static const MenuAction menu_actions[] = {
    action_debug_stats,    /* 0  hidden: count/capacity */
    action_add,            /* 1  */
    action_display,        /* 2  */
    action_sort,           /* 3  */
    action_average,        /* 4  */
    action_find_top,       /* 5  */
    action_search,         /* 6  */
    action_remove,         /* 7  */
    action_risk,           /* 8  */
    action_summary,        /* 9  */
    action_exit            /* 10 */
};

#define MENU_ACTION_COUNT (sizeof menu_actions / sizeof menu_actions[0])

/* ================================ main ================================== */

int main(void)
{
    StudentDB db;
    char line[LINE_LEN];

    if (!db_init(&db))
        return EXIT_FAILURE;

    if (!seed_database(&db))
        return EXIT_FAILURE;          /* db already freed by seed_database() */

    print_header();
    printf("Loaded %zu sample records.\n", db.count);

    while (db.running) {
        int choice;
        int rc;

        print_menu();
        rc = prompt_line("Choose an option (1-10): ", line, sizeof line);
        if (rc == READ_EOF) {
            printf("\nEnd of input: exiting.\n");
            break;
        }
        if (rc == READ_TOO_LONG) {
            printf("Input too long: please enter a menu number.\n");
            continue;
        }
        if (!parse_int(line, &choice) || choice < 0 ||
            (size_t)choice >= MENU_ACTION_COUNT) {
            printf("Invalid choice '%s': enter a number from 1 to 10.\n",
                   line);
            continue;
        }
        menu_actions[choice](&db);
    }

    db_free(&db);
    return EXIT_SUCCESS;
}
