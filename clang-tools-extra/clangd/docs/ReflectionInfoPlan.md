# clangd Reflection Info Plan

## Context

C++26 static reflection deliberately exposes a single opaque value type,
`std::meta::info`, while the represented entity may be a type, declaration,
template, namespace, object, value, base specifier, data-member specification,
annotation, enumerator specification, or attribute. That opacity is correct for
the language model, but it is insufficient for an IDE: seeing only
`std::meta::info` in hover makes reflection-heavy code difficult to inspect.

In this fork, `std::meta::info` is implemented as `decltype(^^int)` in
`libcxx/include/meta`, and the Clang frontend stores concrete reflection values
as `APValue::Reflection`. `ReflectionKind` already carries the semantic class of
the reflected entity, and `APValue` provides typed accessors such as
`getReflectedType()`, `getReflectedDecl()`, `getReflectedTemplate()`,
`getReflectedNamespace()`, `getReflectedBaseSpecifier()`,
`getReflectedDataMemberSpec()`, `getReflectedAnnotation()`, and
`getReflectedEnumeratorSpec()`.

The shortest reliable path is therefore not to decode the library ABI of
`std::meta::info`. clangd should evaluate the expression when this is safe, read
the resulting `APValue::Reflection`, and format a tooling-facing summary.

## Goals

- Show the concrete reflected entity behind `std::meta::info` values in the IDE.
- Keep the first implementation usable through standard LSP hover with no VSCode
  extension changes.
- Avoid forcing template instantiation or evaluating dependent expressions for
  UI purposes.
- Reuse the frontend's `APValue::Reflection` model instead of duplicating
  reflection semantics in clangd.
- Leave room for a second, structured query API that can power a tree view or
  richer client UI.

## Non-Goals

- Do not expose the in-memory representation of `std::meta::info`.
- Do not make hover execute expensive or semantically observable work.
- Do not make inlay hints the primary UI for reflection data; the information is
  too large for inline display.
- Do not require a custom IDE client for the first stage.
- Do not replace standard reflection APIs such as `display_string_of`,
  `identifier_of`, `members_of`, or `annotations_of`.

## Stage 1: Reflection Hover

Stage 1 adds a clangd-local formatter for reflection constant values and uses it
from existing hover paths. The user-facing result should appear when hovering:

- a direct reflect expression such as `^^T`, `^^ns`, `^^Foo::bar`;
- a `constexpr std::meta::info` variable or `auto` variable initialized by a
  reflection expression;
- a non-dependent call result whose value is a reflection, such as
  `std::meta::type_of(r)` when the call is already constant-evaluable;
- an expansion-statement element whose initializer has a non-dependent
  reflection value.

### Display Model

The hover card should keep the existing type/declaration information and add a
reflection section. A compact first version should include:

- `kind`: the `ReflectionKind` rendered as a stable string;
- `display`: a human-readable representation similar in intent to
  `std::meta::display_string_of`;
- `identifier`: present only when the reflected entity has an identifier;
- `type`: present for reflected types, declarations, objects, and values when
  a useful type can be printed;
- `target`: present for declarations, templates, namespaces, parameters, base
  specifiers, and annotations when the target can be named or printed.

Example hover for a reflected type:

```text
std::meta::info

Reflection
kind: type
display: Sora::Kernel::BaseUnknown
type: Sora::Kernel::BaseUnknown
```

Example hover for a reflected member declaration:

```text
std::meta::info

Reflection
kind: declaration
display: Sora::Foo::bar
target: int Sora::Foo::bar
```

Dependent values should not be evaluated. Their hover should remain ordinary
clangd hover, optionally with a conservative note:

```text
std::meta::info

Reflection value is dependent in this template context.
```

### Formatter Boundary

Add a small formatter in clangd, not in `APValue::printPretty`, for the first
stage. `APValue::printPretty` is shared by diagnostics, dumps, and tests; changing
it would have a larger blast radius. clangd hover needs a richer and more
presentation-oriented result, so the first formatter should live near
`Hover.cpp` or a small helper such as `ReflectionInfo.{h,cpp}` under clangd.

Suggested internal shape:

```cpp
struct ReflectionHoverInfo {
  std::string Kind;
  std::optional<std::string> Display;
  std::optional<std::string> Identifier;
  std::optional<std::string> Type;
  std::optional<std::string> Target;
  std::vector<std::pair<std::string, std::string>> Extra;
};
```

The formatter should accept:

```cpp
std::optional<ReflectionHoverInfo>
getReflectionHoverInfo(const APValue &Value, QualType ValueType,
                       const ASTContext &Ctx,
                       const PrintingPolicy &PP);
```

It should return `std::nullopt` unless `Value.isReflection()` is true.

### Evaluation Path

clangd already has `printExprValue()` in `Hover.cpp`, which calls
`EvaluateAsRValue` for non-dependent expressions and stores a string in
`HoverInfo::Value`. Stage 1 should split this into a richer evaluation helper:

- evaluate the selected expression only if it is non-dependent;
- reject function, function pointer, function reference, and void expressions as
  current hover does;
- keep existing guards around struct and union printing;
- before falling back to `APValue::getAsString`, check whether the value is
  `APValue::Reflection`;
- if yes, produce a reflection hover section through the new formatter.

This keeps reflection support aligned with clangd's existing hover evaluation
policy.

### Reflection Kind Formatting

The first stage should cover all current `ReflectionKind` enumerators:

- `Null`: display `null reflection`;
- `Type`: print `QualType` with the AST printing policy;
- `Declaration`: print the qualified declaration name and a compact definition
  or type where available;
- `Template`: print the template name and template kind if available;
- `Namespace`: print the namespace or global namespace;
- `EntityProxy`: print the proxied `UsingShadowDecl`;
- `Parameter`: print the parameter name, function context, and type;
- `BaseSpecifier`: print the base type plus access and virtual-ness;
- `DataMemberSpec`: print the specified type, optional name, alignment,
  bit-width, and `no_unique_address`;
- `Annotation`: print the annotation argument expression and its type;
- `EnumeratorSpec`: print name, optional value, and annotation/attribute counts;
- `Attribute`: print the parsed attribute spelling when available;
- `Object` and `Value`: print the reflected result type and a bounded value
  summary.

If a kind cannot yet produce high-quality text, it should still show the kind and
avoid crashing.

### Integration Points

Likely files:

- `clang-tools-extra/clangd/Hover.cpp`: call the formatter from expression and
  declaration hover paths.
- `clang-tools-extra/clangd/Hover.h`: add a field to `HoverInfo` if the data
  needs to be rendered separately from `Value`.
- `clang-tools-extra/clangd/unittests/HoverTests.cpp`: add hover regression
  tests.
- Optional helper:
  `clang-tools-extra/clangd/ReflectionInfo.{h,cpp}` if keeping the formatter out
  of `Hover.cpp` improves readability.

The formatter should not depend on libc++ implementation details. It should use
Clang AST classes and `APValue` accessors only.

### Tests

Add hover tests for:

- `constexpr std::meta::info r = ^^int;`
- reflected class type;
- reflected function;
- reflected data member;
- reflected class template;
- reflected namespace;
- annotation reflection from a small `[[= ...]]` example;
- base-specifier reflection from `bases_of`;
- `template for` variable of type `std::meta::info`, using the already fixed
  expansion-statement hover/type behavior;
- dependent template context where no evaluation should occur.

The tests should assert both the ordinary type line and the reflection section.

### Acceptance Criteria

- Hovering direct and stored `std::meta::info` values shows more than just
  `std::meta::info`.
- Dependent reflection expressions do not trigger evaluation crashes or expensive
  instantiation.
- Existing non-reflection hover behavior remains unchanged.
- `clangd --check` on Sora reflection-heavy headers still builds preamble and AST
  without frontend crashes.

## Stage 2: Structured Reflection Query

Stage 2 adds an optional clangd extension for clients that want a tree view or
inspectable data model. This should be designed after Stage 1 has stabilized,
because the hover formatter will identify the exact internal fields worth
exposing.

### Request Shape

Proposed method:

```text
textDocument/reflectionInfo
```

Request:

```json
{
  "textDocument": { "uri": "file:///..." },
  "position": { "line": 10, "character": 15 },
  "maxDepth": 2,
  "maxChildren": 64,
  "include": {
    "members": true,
    "bases": true,
    "annotations": true,
    "templateArguments": true,
    "layout": true,
    "sourceLocation": true
  }
}
```

Response:

```json
{
  "kind": "type",
  "display": "Sora::Kernel::BaseUnknown",
  "identifier": "BaseUnknown",
  "type": "Sora::Kernel::BaseUnknown",
  "sourceLocation": {
    "uri": "file:///G:/Teaching/Vulkan/Sora/include/...",
    "range": { "start": { "line": 1, "character": 0 },
               "end": { "line": 1, "character": 1 } }
  },
  "children": [
    { "role": "member", "kind": "declaration", "display": "Query" },
    { "role": "annotation", "kind": "annotation", "display": "..." }
  ]
}
```

### Data Model

The structured response should be bounded and explicit:

- `kind`: same stable names as Stage 1;
- `display`: human-readable summary;
- `identifier`: optional;
- `type`: optional;
- `target`: optional;
- `sourceLocation`: optional;
- `layout`: optional `{ "size": ..., "alignment": ..., "offset": ... }`;
- `children`: optional recursive entries, each with a `role`;
- `truncated`: boolean when `maxDepth` or `maxChildren` cuts output;
- `diagnostics`: non-fatal reasons why a requested child category is unavailable.

### Child Categories

The initial extension can expose:

- `templateArguments` from template specializations and substituted templates;
- `members` for class and namespace reflection values;
- `bases` for class reflection values;
- `annotations` for declarations and types;
- `enumerators` for enum type reflections;
- `parameters` and `returnType` for function reflections;
- `layout` for types and fields when AST layout is available.

Stage 2 should not blindly call arbitrary standard-library metafunctions. It
should prefer direct AST/APValue data and only use constant-evaluation paths that
clangd already trusts.

### Client Strategy

The first client can be VSCode-specific, but the server API should be client
agnostic. A plain client can still use hover from Stage 1. A richer client can
show:

- root reflected entity;
- expandable children for members, bases, annotations, template arguments;
- source navigation links for declarations;
- bounded layout metadata.

### Tests

Add protocol tests for:

- direct `^^T`;
- `constexpr std::meta::info` variable;
- no result on non-reflection expression;
- dependent expression result with a diagnostic but no crash;
- truncation behavior;
- nested members and annotations with bounded depth.

### Acceptance Criteria

- The extension returns deterministic JSON for non-dependent reflection values.
- The response is bounded by `maxDepth` and `maxChildren`.
- Missing information is reported as structured diagnostics instead of server
  errors.
- Standard hover remains the fallback for clients that do not support the
  extension.

## Risks and Mitigations

- **Risk: accidental expensive evaluation.** Only evaluate expressions that the
  current hover path already considers safe, and reject dependent expressions.
- **Risk: unstable textual output.** Use existing `PrintingPolicy` and keep
  tests focused on stable summaries, not full AST dumps.
- **Risk: too much data in hover.** Keep Stage 1 compact. Large recursive data
  belongs in Stage 2.
- **Risk: frontend representation drift.** Centralize reflection formatting in
  one helper that switches over all `ReflectionKind` values and fails closed.
- **Risk: client incompatibility.** Stage 1 uses standard LSP hover; Stage 2 is
  optional.

## Recommended Implementation Order

1. Add `ReflectionHoverInfo` and a formatter that switches over `APValue`
   reflection values.
2. Wire the formatter into expression hover while preserving current
   `printExprValue` behavior for non-reflection values.
3. Add hover tests for direct `^^` and `constexpr std::meta::info` variables.
4. Expand kind coverage to declarations, templates, namespaces, base specifiers,
   annotations, and data-member specs.
5. Run clangd hover tests and targeted `clangd --check` on Sora reflection-heavy
   headers.
6. Only after hover behavior is stable, add the optional
   `textDocument/reflectionInfo` request and a bounded JSON response model.

