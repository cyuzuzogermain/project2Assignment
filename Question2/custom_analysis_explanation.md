# Custom Analysis: Lexical Diversity Score (LDS)

Program: **Dynamic Text Mining and Linguistic Pattern Analyzer**
Student ID: 12345 — menu option **10**, function `lexical_diversity_analysis()`.

---

## 1. What the score is for

The **Lexical Diversity Score** is the *type-token ratio* expressed as a
percentage. It answers a simple question: **how much of the vocabulary does
this text actually reuse?**

* **Tokens** = total words written (`total_words`).
* **Types** = distinct words after normalisation (`distinct_words`).

A text that says the same few words over and over scores low; a text that
introduces a new word almost every time scores high. The score is a quick,
objective indicator of vocabulary variety for a short passage — it says
nothing about meaning or quality, only about repetition.

---

## 2. Formulas and a worked example (the real default paragraph)

Input: the default Austen paragraph (menu option 1), which the program
reports as **168 total words** and **104 distinct words**.

### 2.1 Lexical Diversity Score

```
LDS = distinct_words / total_words * 100
    = 104 / 168 * 100
    = 61.9047...  ->  61.90 %
```

### 2.2 Repetition Intensity

How much of the vocabulary is *reused* (words that appear 2 or more times).

```
Repetition Intensity = (distinct words occurring 2+ times) / distinct_words * 100
                     = 24 / 104 * 100
                     = 23.0769...  ->  23.08 %
```

### 2.3 Hapax ratio

How much of the vocabulary appears *exactly once* (hapax legomena).

```
Hapax ratio = (distinct words occurring exactly once) / distinct_words * 100
            = 80 / 104 * 100
            = 76.9230...  ->  76.92 %
```

Note `repeated + hapax = distinct`: `24 + 80 = 104`. ✓

### 2.4 Average word length

```
Average word length = total letters in all words / total_words
                    = 793 / 168
                    = 4.7202...  ->  4.72 characters
```

### 2.5 Top 5 most repeated words (default paragraph)

| # | Word | Count |
|---|------|-------|
| 1 | his  | 11 |
| 2 | the  | 8 |
| 3 | and  | 7 |
| 4 | a    | 6 |
| 5 | of   | 5 |

(`of` and `was` both occur 5 times; ties are broken alphabetically, so `of`
comes first — the same rule `qsort` uses in option 7.)

---

## 3. Interpretation guide

**These thresholds are heuristics for short passages.** The type-token ratio
is length-dependent: as a text gets longer the score *falls*, because a
writer runs out of new words and starts reusing the common ones. Two texts
of different length are therefore not directly comparable — compare texts
of similar length, or treat the band as a rough signal only.

| LDS | Interpretation |
|-----|----------------|
| **at least 75** | very varied vocabulary |
| **55 to 74.99** | moderately varied |
| **below 55** | repetitive |

Default paragraph: **61.90 % → moderately varied** (168 words is a short
passage, so the band is a rough signal).

Supporting readings for the same text:

* Repetition intensity 23.08 % — about a quarter of the vocabulary is
  reused; three quarters appear once.
* Hapax ratio 76.92 % — consistent with a short narrative passage that
  keeps introducing new content words.
* Average word length 4.72 characters — typical of English prose.

---

## 4. Word rules used by every operation

These rules are identical in counting, statistics, sorting, search, the
summary and this custom analysis (documented at the top of `main.c`).

1. **A word is a run of ASCII letters and digits.** Bytes `>= 0x80`
   (**every non-ASCII byte**) are treated as separators, so `café` splits at
   the first non-ASCII byte.
2. **Case:** letters are converted to lower case with `tolower()` before the
   word is stored, so `The`, `THE` and `the` are one word (`the`).
3. **Punctuation:** punctuation at the start or end of a word is stripped —
   `. , ; : ! ?` quotes and brackets, so `(brackets)...` becomes `brackets`
   and `"quoted"` becomes `quoted`.
4. **Apostrophes:** an apostrophe *inside* a word is kept, so `don't` and
   `it's` stay one word each. A leading or trailing apostrophe is stripped
   (it acts as a separator), so `'tis` becomes `tis`.
5. **Hyphens:** a *single* hyphen inside a word is kept, so
   `brother-in-law` and `good-looking` are one word each. A **double hyphen
   `--`** or a hyphen with a space around it is a separator, so
   `stop--it's` gives `stop` and `it's`.
6. **Whitespace:** repeated spaces, tabs and newlines collapse to a single
   separator.
7. **Truncation:** a word longer than **39 characters** is truncated to its
   first 39 characters when it is stored in `struct wordStat.name[40]`, and
   `truncated_words` is incremented. `longest_word()` still reports the
   *true* length, because it walks the raw text with a `const char *`
   pointer instead of reading the truncated statistics array.
8. **Average word length** uses the true (untruncated) word length.

---

## 5. Edge cases handled

* `total_words == 0` or `distinct_count == 0` → no division by zero; the
  analysis reports that there is nothing to analyse.
* No text loaded → option 10 prints a clear message instead of dereferencing
  a null pointer.
* All four metrics are recomputed from `TextAnalyzer` on every call; no
  state is cached between calls, so a sorted statistics array cannot skew
  the result.
