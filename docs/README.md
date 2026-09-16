# Birdcher docs

Headless bird-observation camera on a Khadas VIM3 (Amlogic A311D), Armbian
26.8.3, kernel `6.18.44-current-meson64`.

**Start here:** [STATUS.md](STATUS.md) — what works today, in one page.
Then [ROADMAP.md](ROADMAP.md) for the phase plan.
For architectural review of the current proof, see
[ARCHITECTURE_REVIEW_2026-09-16.md](ARCHITECTURE_REVIEW_2026-09-16.md).

## Layout

| Path | What it holds | Read it when |
| --- | --- | --- |
| [STATUS.md](STATUS.md) | current state, one page | always, first |
| [ROADMAP.md](ROADMAP.md) | phases 0-10, goal and acceptance criteria | planning the next step |
| `camera/` | the camera stack as it now is | working on capture or image quality |
| `kernel/` | which kernel, which headers, why no rebuild | build or module-loading trouble |
| `hardware/` | board, sensor and connector facts; datasheets | wiring, DT, or "what is this pin" |
| `tasks/` | one file per worked task, kept as evidence | picking up unfinished work |
| `archive/` | superseded process logs and finished task prompts | historical curiosity only |
| `logs/`, `images/` | raw captures referenced by tasks | checking someone's evidence |

## The rule that matters

`AGENTS.md` at the repo root: **before inventing a fix, diff against Khadas
armisp-g12b.** It is a pinned submodule at `external/khadas-common_drivers`.
That rule has settled three separate bugs that reasoning alone did not.
