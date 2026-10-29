# Project Workspace

This workspace contains three small C command-line tools built with `gcc -std=c11 -Wall -Wextra`.

## Question 1 — Dynamic Academic Performance and Risk Analysis Engine

`Question1/main.c` is a menu-driven student record program. It keeps a dynamically growable/shrinkable array of student records, seeds itself with 12 sample students, and exposes operations such as add, display, sort, average, search, remove, and a custom **Academic Risk Index** analysis.

- Build: `cd Question1 && gcc -Wall -Wextra -std=c11 -o engine main.c`
- Run: `./engine`

## Question 2 — Dynamic Text Mining and Linguistic Pattern Analyzer

`Question2/main.c` is a text-analysis program. It can load a built-in Austen paragraph or accept user text, then count words, list word statistics, sort them, search for words, and run a custom **Lexical Diversity Score** analysis. It also has a memory inspection view.

- Build: `cd Question2 && gcc -Wall -Wextra -std=c11 -o analyzer main.c`
- Run: `./analyzer`

## Question 3 — Dynamic IoT Event Monitoring and Callback Engine

`Question3/main.c` is a device/reading monitor. It registers devices, generates readings, processes them through a callback dispatch table, and runs a custom **Sensor Drift Detector** callback. It also supports an auto-simulation mode.

- Build: `cd Question3 && gcc -Wall -Wextra -std=c11 -o main main.c`
- Run: `./main <seed>`, for example `./main 2024`

## Notes

- Each tool prints its own usage in its menu.
- Sample runs and evidence files live alongside the source in each question folder.
