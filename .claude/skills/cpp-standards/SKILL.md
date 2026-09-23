---
name: cpp-standards
description: xmatch C/C++ formatting and mechanical coding standard — indentation, line length, include guards and ordering, namespace closing, naming, casts, integer types. Apply when writing or editing .cpp/.hpp/.c/.h files, when adding new files, and during code review. Design/performance/security principles are out of scope for this skill — they belong to the best-practices skill.
disable-model-invocation: true
allowed-tools: Read
---

# C/C++ coding standard

The rules were derived from the existing code; the codebase passes all of them today.

## Formatting

1. Indent with 4 spaces. No tabs.
2. Lines ≤ 110 columns. (`tests/` is exempt: GoogleTest macros are naturally long.)
3. Headers are guarded with `#pragma once`; `#ifndef` include guards are not used.
4. Include order — one blank line between groups:
   own header (in a `.cpp`) → `<standard>` → `"xmatch/..."` → `"engine/..."`.
5. `namespace` and `extern "C"` blocks end with a closing comment:
   `} // namespace xmatch::detail`, `} // extern "C"`.

## Naming

6. Types `PascalCase`, functions and variables `snake_case`, member fields with a
   trailing `_`, `constexpr` constants and enum values `kCamelCase`. No macros —
   except `XMATCH_EXPORT` in `include/xmatch/matching_engine_api.hpp`: symbol
   visibility is an attribute on a declaration, which no C++ construct other than
   a macro can abstract.

## Types and conversions

7. No C-style casts; use `static_cast`. `reinterpret_cast` and
   `const_cast` only with a comment stating the justification.
8. Every integer that is stored or crosses an interface is fixed-width
   (`std::uint32_t`, `std::int64_t`, `std::size_t`). A bare `int` is acceptable
   for a short-lived local shift counter.
9. Standard library over compiler extensions: `std::countl_zero` (`<bit>`),
   not `__builtin_clzll`.
10. Non-trivial parameters are passed by `const T&`; non-mutating
    methods are marked `const`.

## Checking

```sh
.claude/skills/cpp-standards/scripts/check_style.sh          # changed files
.claude/skills/cpp-standards/scripts/check_style.sh <file>   # a single file
```

The script mechanically verifies rules 1, 2, 3, 5, 7 and 9. Rules 4, 6, 8 and 10
can't be reliably distinguished with regexes (they produce false positives) — they
are applied by hand while writing and during review.

The script also checks items 7, 14 and 16 of the `best-practices` skill, to the
extent they can be measured mechanically, and reports that item number on a
violation. The items' text is not repeated here; read
`best-practices/references/practices.md` for the rationale.
