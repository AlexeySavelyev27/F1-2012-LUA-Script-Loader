# Guidelines for Codex agents

This repository contains a Windows DLL project for the game F1 2012.
It is written mostly in C++ with some Lua scripts for plugins.

## Coding style
- Use four spaces for indentation.
- Place opening braces on the same line as the statement.
- End every file with a newline.
- Avoid trailing whitespace.

## Contribution rules
- Do not commit build artefacts from `Release/` or `Debug/`.
- Keep commit messages short: a one line summary under 50 characters
  and an optional body wrapped at 72 characters.

## Testing
There is no automated test suite. After making changes ensure `git status`
shows only the intended files are modified.
