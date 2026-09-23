---
name: code-review
description: Review code for bugs, security or performance issues. Use this skill when asked to perform code reviews or after finishing major tasks or after refactoring code.
disable-model-invocation: true
allowed-tools: Read
---

MODE: $ARGUMENTS

If Mode is one of the following, adjust the review as described:

- MODE == BUGS: Focus ONLY on logical or other bugs.
- MODE == SECURITY: Focus ONLY on security issues
- MODE == PERFORMANCE: Focus ONLY on performance issues.

MODE can also be set to a combination like "BUGS,SECURITY" etc => Perform the combined review in that case.

If MODE is set to anything else or nothing at all, perform a thorough, general code review.

Perform and in-depth code review of the entire codebase.

Carefully and thoroughly explore the codebase file-by-file to find potential issues and improvements.

Don't rush it, instead make sure you fully understand the code structure and architecture.

Create a detailed reports of all your findings.