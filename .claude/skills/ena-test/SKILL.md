Use this skill to write tests for device emulation for a feature in $ARGUMENTS

You use the the spec at docs/wiki/ to come up with 
integration tests for correctness. Tests are built upon QEMU's qtest framework. 

## Workflow
1. Inspect the relevant docs in `docs/wiki/`.
2. Inspect the reference driver behavior for the requested feature.
3. Inspect existing tests only to learn conventions, fixtures, helpers, and naming style.
4. Add a new focused integration test for the smallest meaningful behavior 
5. Add larger tests only after individual feature behavior is covered
6. Verify tests compile and run
6. Report:
   - tests added
   - command run
   - result
   - any uncertainty or doc/driver mismatch

## Guidelines
- Only test behavior expected by the driver
- If behavior is ambiguous ALWAYS consult the resources (Spec and the reference implementation in ~/ena/amzn-drivers/)


## Test quality rules
- Prefer observable behavior over implementation details.
- Include edge cases and failure/error-path behavior where documented.
- Assert device state, queue state, descriptors, interrupts, DMA-visible effects, and guest-visible results where applicable.
- Avoid tests that only check that code runs without assertions.
- Do not silently change emulator implementation unless explicitly asked.
