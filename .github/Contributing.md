# Contributing to Jsonifier

Thanks for your interest in improving Jsonifier. This guide covers how to report problems, propose changes, and get a pull request merged.

## Reporting Bugs

Open an issue and include:

- A minimal JSON input and C++ type that reproduce the problem.
- Compiler and version, operating system, and target architecture.
- Expected behavior versus actual behavior.
- For performance regressions, the benchmark used and the numbers before and after.

## Proposing Changes

For anything larger than a small fix, open an issue first so the approach can be discussed before you invest time in it.

1. Fork the repository and create a branch from `main`.
2. Make your change.
3. Build and run the unit tests locally (see below).
4. Open a pull request against `main` describing what changed and why.

## Building and Testing

Jsonifier is built with CMake; presets are in `CMakePresets.json`. Tests live in `unit-tests`.

Every pull request is built by CI on the following matrix, and all of it must pass:

| OS      | Compilers |
|---------|-----------|
| Ubuntu  | Clang, GCC |
| macOS   | Clang, GCC |
| Windows | MSVC |

The SIMD backend is selected at compile time, so please test on the hardware you have and note which backend your change touches.

## Code Style

- Run `run-clang-format.ps1` before committing; formatting follows the repository `.clang-format`.
- Do not add explanatory comments to code. Write self-documenting code instead. Comments that attribute algorithms, techniques, or implementations to their original authors or sources are explicitly permitted. If current-us cannot write self-documenting code, then future-us does not deserve to understand it.
- Keep includes lean. Only include what the file actually uses.
- Keep changes focused. Unrelated refactors belong in their own pull request.

### Member Ordering

Every struct and class orders its members as follows, applied separately within each `public:`, `protected:` and `private:` section so no member changes access:

1. Type aliases (`using ... =`), in the order the standard library uses for the same names. Iterators: `iterator_concept`, `iterator_category`, `element_type`, `value_type`, `difference_type`, `pointer`, `reference`. Containers: `value_type`, `allocator_type`, `size_type`, `difference_type`, `reference`, `const_reference`, `pointer`, `const_pointer`, `iterator`, `const_iterator`, `reverse_iterator`, `const_reverse_iterator`. Project-specific aliases sit directly after the standard alias they extend (for example `r_reference` after `reference`).
2. Default constructor.
3. Parameterized constructors.
4. Move assignment operator.
5. Move constructor.
6. Copy assignment operator.
7. Copy constructor.
8. All other functions, in descending order of the character count of each function's longest line.
9. Destructor.
10. Data members, ordered first to minimize padding (descending alignment), then in descending order of the character count of each member's line.

Nested types, enums, and `static constexpr` members stay above the functions, since signatures and other declarations may name them. A `static constexpr` member whose initializer calls a member function goes below the data members, after the function it calls. Adjacent one-line `= default;` and `= delete;` declarations stay together without blank lines between them.

When reordering data members, update everything that depends on their order: member initializer lists, positional aggregate initialization (`return { ... }`, `value = { ... }`, `type{ ... }`), and designated initializers, which must follow declaration order.

## Performance Changes

Jsonifier is performance-sensitive. A change on a hot path (parsing, serialization, SIMD or SWAR routines) should come with benchmark results on at least one compiler, and should state any compiler where it is slower. Changes that win on one compiler but regress another need a per-compiler split to be accepted.

## Pull Request Checklist

- [ ] Builds and passes tests on all CI platforms
- [ ] Formatted with clang-format
- [ ] Struct and class members follow the member ordering rules
- [ ] No added explanatory code comments (source attributions are permitted)
- [ ] Benchmarks included for performance-sensitive changes
- [ ] Linked to the issue it addresses, if any

## Licensing

By contributing, you agree that your contributions are licensed under the project's MIT license (see `License.md`).
