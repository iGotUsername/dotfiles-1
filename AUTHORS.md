# Authors and attribution

## Personal setup

**iGotUsername** owns and maintains this personal dotfiles repository and supplied
the original desktop setup, preferences, configuration, and project direction.
The pre-rewrite Python everbar is retained in `tests/reference_everbar.py` as the
original rendering reference; it is not claimed as AI-authored here.

## AI-authored implementation

At the maintainer's request, **GPT-6.1 Sol via OpenCode** wrote the native C
everbar rewrite, its new tests and documentation, and the repository-wide
installation/backup/restore tooling and associated tests and documentation.
The maintainer tested the C version on their Void Linux desktop and confirmed
that the appearance and controls work.

This is explicit attribution of AI-generated work, not a claim that the
maintainer manually wrote that implementation, nor a claim of endorsement by
OpenAI, OpenCode, or the upstream projects. Tests describe the checks performed;
they are not a guarantee that software is free of defects.

Commits made by the coding agent use the author name **GPT-6.1 Sol via OpenCode**.
The address `gpt-6.1-sol@opencode.invalid` is a deliberately non-deliverable
placeholder, not a real person's email address or a GitHub account.

## Upstream projects

- **dwl** — https://codeberg.org/dwl/dwl
- **dwlb** — https://github.com/kolunmi/dwlb
- **Everforest**, by **sainnhe** and contributors — https://github.com/sainnhe/everforest

Original copyright/license notices are retained. The upstream revisions used by
the installer are pinned in `sources.lock.json` and listed in `SOURCE-BASES.txt`.
The personal configurations and changes do not transfer authorship of those
upstream projects. See `LICENSE` and the upstream license files under `src/`.
