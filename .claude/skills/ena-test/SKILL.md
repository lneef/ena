Use this skill whenever you want to add new integration tests for ena. 
The scope of the integrations tests must be provided via $ARGUMENTS.
Use the provided reference resources to implement correctness tests.
Tests are built upon QEMU's qtest framework.

## Reference Resources
- Use the **spec** in `docs/wiki/` as primary reference point for desired device behavior
- If the **spec** lacks details escalate to the reference implementation  

## Guidelines
- Only test behavior expected by the driver
- If behavior is ambiguous ALWAYS consult the resources (**spec** and the reference implementation in ~/ena/amzn-drivers/)
- you MUST NOT make any assumptions. If anything is ambiguous, add it to a `TODO.md` file in tests/. Structure for each entry:
    - Write a short description of the feature (1 sentence)
    - List the missing pieces in bullet points

## Workflow
1. Inspect the relevant resources for desired device behavior.
    - if you resolve entries of `TODO.md`, clean them up in `TODO.md`
2. Inspect existing tests to learn conventions, helpers, and naming style.
3. Add a new focused integration test for the smallest meaningful behavior 
4. Add larger tests only after individual feature behavior is covered
5. Verify tests comply with the references resources 
    - Fix deviations from the reference resources at this point
6. If `TODO.md` is non empty go back to `1.`. With `TODO.md` as feature set to test
7. Verify tests compile and run
8. Report:
   - tests added
   - command run
   - result
   - any uncertainty or doc/driver mismatch

## Test quality rules
- Prefer observable behavior over implementation details.
- Include edge cases and failure/error-path behavior where documented.
- Assert device state, queue state, descriptors, interrupts, DMA-visible effects, and guest-visible results
- Avoid tests that only check that code runs without assertions.
- DO NOT change emulator implementation unless explicitly asked
