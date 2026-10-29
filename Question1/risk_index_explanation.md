# The Academic Risk Index

Custom analysis for the **Dynamic Academic Performance and Risk Analysis Engine**
(Student ID: 12345, `main.c`, menu option **8**).

## Purpose

The Academic Risk Index is an **early-warning score** that shows how far a
student's grade sits below a *safe* level (the risk threshold).

A flat "below 60?" test treats a grade of 58 and a grade of 20 as the same
problem, even though the second student is in far deeper trouble. The index
converts that distance into a single 0-100 number, so a small shortfall and a
critical one are never ranked equally. It is meant for triage: who needs help
first, and how urgently.

Two different scales are in play and they must not be confused:

| Scale | Value | Where it is used |
|---|---|---|
| Risk threshold | **60** by default (user may enter 1-100) | Academic Risk Index only |
| Pass mark | **50** (fixed) | Summary report ("at or above pass mark") |

## Formula

For each student, with `T` = risk threshold:

```
if grade >= T:   risk index = 0
else:            risk index = (T - grade) / T * 100
```

- `0` means the student is at or above the safe level (not at risk).
- `100` means a grade of 0 with any threshold.
- The score grows *linearly* with the shortfall, and is scaled by the
  threshold itself so it stays in 0-100 for any threshold the user picks.

## Bands

| Risk index | Band |
|---|---|
| exactly 0 | **Safe** |
| above 0 up to 25 | **Low risk** |
| above 25 up to 50 | **Moderate risk** |
| above 50 | **High risk** |

Band edges are inclusive at the top: a risk index of exactly 25.0 is *Low
risk*, exactly 50.0 is *Moderate risk*.

## Worked example (default threshold 60)

| Grade | Calculation | Risk index | Band |
|---|---|---|---|
| 92 | `grade >= 60` | 0.0 | Safe |
| 58 | `(60-58)/60*100` | 3.3 | Low risk |
| 55 | `(60-55)/60*100` | 8.3 | Low risk |
| 45 | `(60-45)/60*100` | 25.0 | Low risk |
| 42 | `(60-42)/60*100` | 30.0 | Moderate risk |
| 30 | `(60-30)/60*100` | 50.0 | Moderate risk |
| 27 | `(60-27)/60*100` | 55.0 | High risk |
| 20 | `(60-20)/60*100` | 66.7 | High risk |
| 0  | `(60-0)/60*100`  | 100.0 | High risk |

The same formula with a custom threshold of **75** and grade 20 gives
`(75-20)/75*100 = 73.3` (High risk), *higher* than the 66.7 it scores at
threshold 60: raising the safe level makes the same grade sit further below it.

## What the report prints

Menu option **8** outputs:

1. The threshold in use (default 60, blank input keeps it) and the pass mark.
2. A table of **every at-risk student** (risk index above 0), sorted by risk
   index **descending**: ID, name, course, grade, risk index, band. The most
   urgent student is always the first row.
3. The **count of students in each band** over the whole class.
4. The **class-wide average risk index** (all students, safe ones contributing
   0), out of 100.
5. A printed explanation of purpose, formula, bands and how to read it.

The summary report (option **9**) reuses the same calculation with the default
threshold of 60 so the two screens always agree.

## How to interpret the results

- **0 / Safe** - at or above the safe level; no action needed from this tool.
- **Low risk (0-25]** - slightly below the safe level. Worth monitoring;
  a few extra marks usually clears it.
- **Moderate risk (25-50]** - clearly below the safe level. A revision plan
  or tutoring is justified.
- **High risk (>50)** - far below the safe level; the index approaches 100 as
  the grade approaches 0. These students need intervention first.
- **The class average** summarizes the cohort: a class average of 23 means the
  typical student sits about 23% of the threshold below the safe level. Use it
  to compare cohorts or terms, not to judge an individual.
- Because the score is *relative to the threshold*, changing the threshold
  changes every number. A stricter threshold (say 80) flags more students and
  raises their scores; a lenient one (50) flags fewer. Always read the printed
  threshold next to the results.

## Limitations

- The index only looks at the grade, not at credits, weighting, attendance or
  how close the student is to other thresholds such as the pass mark.
- It is linear: the difference between 10 and 20 points of shortfall is always
  the same, regardless of where it falls.
- The threshold is a policy choice; it has no meaning outside the number the
  operator picked for that run.
