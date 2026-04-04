# C++ Documentation Standard

This document defines how `anolis` documents C++ source in a way that helps
human maintainers first and generated API docs second.

The goal is not maximum comment coverage. The goal is to make contracts,
threading, ownership, lifetime, and non-obvious behavior easy to find without
burying the code in boilerplate.

## Principles

- Prefer documenting contracts over narrating implementation steps.
- Document facts that are easy to forget and expensive to rediscover.
- Keep comments lean enough that contributors will maintain them.
- If a comment only paraphrases a symbol name, delete it.
- Use one structured style across the repo: Doxygen-compatible `/** ... */`
  block comments.

## Canonical Structured Comment Style

Use `/** ... */` for structured API documentation.

Do not introduce `///` as a second style.

Put structured API comments on declarations in headers by default. Avoid
duplicating the same contract comment on the out-of-line definition in a `.cpp`
file unless the implementation needs additional algorithm context.

Use these tags when they add value:

- `@file` for contract headers and genuinely complex implementation files
- `@brief` for a one-line summary
- `@param` when a parameter has non-obvious meaning, units, ownership, or
  preconditions
- `@return` when success/failure semantics are not obvious from the type alone

Prefer short prose sections for important operational details:

- `Threading:`
- `Lifetime:`
- `Ownership:`
- `Invariants:`
- `Error handling:`
- `Usage:` or `Example:`

These labels should stay plain and repeatable. Do not invent a large custom tag
vocabulary.

## What Must Be Documented

Structured comments are required for:

- public or cross-module interfaces
- stateful classes with lifecycle, locking, caching, supervision, or restart
  semantics
- config structs and enums with non-obvious units, defaults, or cross-field
  constraints
- functions whose signatures do not fully communicate blocking behavior,
  partial-failure behavior, ownership transfer, cache consistency, or required
  call order

Field-level comments are required only when a field carries non-obvious
semantics, unit conventions, lifecycle meaning, or cross-field constraints.

## What Is Optional

Structured comments are optional for:

- internal helper types whose contracts are already obvious from surrounding
  code
- small private helpers that do one straightforward thing
- implementation files that do not carry tricky concurrency, validation, or
  state-machine behavior

When in doubt, prefer a short invariant or behavior note over a long walkthrough.

## What Should Usually Be Omitted

Do not add structured comments for:

- trivial getters and setters
- constructors or destructors whose behavior is obvious from the type
- comments that restate names, types, or units already obvious from the
  signature
- every field in a simple data carrier struct
- line-by-line implementation narration that the code already explains well

If the code is hard to read because of naming or structure, improve the code
first and add comments second.

## Header and Source File Guidance

Headers are the primary home for API contracts.

Use file-level `@file` comments in headers when the file defines a major
contract surface or a type family with shared semantics.

Use file-level `@file` comments in `.cpp` files only when the implementation
contains behavior worth orienting readers to before they scan the code. Typical
examples include:

- restart or supervision flows
- polling or cache-consistency logic
- lock ordering or thread-handoff behavior
- error aggregation or validation passes

Implementation comments in `.cpp` files should explain invariants and design
constraints, not narrate each statement.

## Tone and Content

Comments should be concise, factual, and stable.

Good comments usually answer one or more of these questions:

- What contract does this type or function enforce?
- What must callers do before or after calling it?
- What thread or lifecycle assumptions matter?
- Who owns the referenced object or resource?
- What failure modes or degraded states should a maintainer expect?

Prefer present-tense, direct language. Avoid marketing language, speculation,
and implementation diary text.

## Duplication Rules

Repo docs explain architecture and workflows.

Code comments explain local contracts close to the code that enforces them.

Do not copy long architecture explanations into headers. Summarize the local
contract and let the higher-level docs carry the broader system narrative.

## Local API Docs

Generate local API docs from the repo root with:

```bash
doxygen docs/Doxyfile
```

Generated HTML goes to `build/docs/doxygen/html/` and should never be
committed.

## Examples

Good:

```cpp
/**
 * @brief Registry of live devices known to the runtime.
 *
 * Threading:
 * Safe for concurrent reads and registry updates.
 *
 * Invariants:
 * Device handles are unique by provider/device pair.
 */
class DeviceRegistry {
```

Good:

```cpp
/**
 * @brief Restart a provider and validate the replacement inventory before swap.
 *
 * Error handling:
 * Returns false and preserves the previous registry view if rediscovery or
 * ownership validation fails.
 */
bool restart_provider(const std::string& provider_id, std::string& error);
```

Avoid:

```cpp
/** @brief Gets the device registry. */
DeviceRegistry& registry();
```

Avoid:

```cpp
/**
 * @brief Set polling interval.
 * @param interval_ms Polling interval in milliseconds.
 */
void set_polling_interval(int interval_ms);
```

The second example is not useful unless the function has additional rules such
as accepted ranges, locking requirements, or delayed effect.

## Review Checklist

Before keeping a new comment, ask:

1. Is the contract unclear from the signature and surrounding names alone?
2. Does the comment describe a real invariant, precondition, ownership rule, or
   failure mode?
3. Is the same fact already obvious from a nearby type name or higher-level doc?
4. Would a future maintainer realistically keep this comment up to date?

If the answer to most of these is "no", the comment probably should not exist.
