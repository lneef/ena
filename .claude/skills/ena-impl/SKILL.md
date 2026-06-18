use this skill to implement new features in the device emulation layer. Features are passed via $ARGUEMNT

The spec in docs/wiki/ serves as reference
## Workflow
- Divide the feature in to implementation steps e.g.
- For each step:
    - Locate the desired behavior in the spec
    - Implement each step as specified by the spec.
- Use @spec-verifier to evaluate whether your implementation complies with the spec
- Locate the tests for the features in tests/ and verify the pass

## Guidelines
- If anything is ambiguous or not clearly document, inspect the reference again till you have a clear idea of the semantics
