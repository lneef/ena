---
name: ena-impl
descritption: This skill specifies how device emulation is implemented for the ENA-NIC. Use it whenever you want to implement a new feature or adapt an exisiting one
---

The **spec** in docs/wiki/ and the reference implementation for the driver are the Ground Truth. The workflow itself is Test-Driven. 
At first you implement tests for the new features then the actual feature.
## Workflow
- Divide the feature set of ENA into implementation steps 
- For each step:
    - Locate the desired behavior in the Ground Truth and update the **spec**
    - Implement meaningful tests for the new feature using @ena-test
    - Implement each step as specified by the spec

## Guidelines
- If anything is ambiguous or not clearly document, inspect the reference again till you have a clear idea of the semantics
- The Goal of Project is to have a completely emulated ENA-NIC that can forward/receive packets via a tun interface
