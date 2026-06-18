Research ENA reference semantics for: $ARGUMENTS

Act as coordinator.

Split the request into independent shards and run parallel inspector agents where useful:
- registers / MMIO
- admin queue
- TX descriptors / LLQ
- RX descriptors / completions
- interrupts / AENQ
- queue setup
- RX-Path / TX-Path
- RSS 

Each inspector must update docs/wiki/ before returning. Each parallel agent runs on its own worktree

After inspectors finish:
1. Inspect each subagent summary.
2. Locate each worktree/branch created by the subagent.
3. For each worktree:
   - review `git status`
   - review `git diff`
   - merge, rebase or apply the resulting test file into the main tree
4. Resolve conflicts.
5. Run wiki-verifier on touched pages.
6. If verification fails, ask inspectors to patch only failed claims/update the wiki.
7. Compose final answer using only wiki-backed claims.

Final response format:
- Wiki pages outlining the answer to the research questions
