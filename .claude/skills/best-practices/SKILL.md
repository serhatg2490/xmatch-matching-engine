---
name: best-practices
description: Good programming practices in xmatch — design principles, performance and scalability, readability, security and robustness. Use when writing or editing C++ code (.cpp/.hpp), refactoring, adding a new module/class, doing code review, or justifying a design decision. Formatting rules (indentation, naming, casts, include order) are out of scope for this skill — they belong to cpp-standards.
disable-model-invocation: true
allowed-tools: Read
---

# Good programming practices

When writing, refactoring or reviewing C++ code, make decisions under these four
headings:

1. **Design principles** — distribution of responsibilities, interface width,
   where information is defined.
2. **Performance and scalability** — complexity, hot-path cost,
   data layout, measurement-driven decisions.
3. **Readability** — control flow, naming, level of abstraction,
   the purpose of comments.
4. **Security and robustness** — input validation, integer arithmetic, bounds
   safety, error visibility.

## How to use

The full list of items, with bad/good examples for each, is in
[`references/practices.md`](references/practices.md).
When applying a practice, weighing a design decision or reviewing code,
**read that file first**, then cite the relevant item by its number as the
justification.

If an item conflicts with the invariants in this project's `CLAUDE.md`,
`CLAUDE.md` wins; this file generalizes it, it does not replace it.
