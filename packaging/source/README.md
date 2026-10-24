# Local source snapshot

Create a preview snapshot of the current Git worktree (tracked files plus nonignored untracked files):

```bash
python packaging/source/package.py --source-dir . --output-dir /tmp/mikai-source-preview
```

The output directory must be new. The generator refuses overlap with the source tree, excludes ignored files and build/user-state/release output, and fails closed on tracked sensitive paths and symlinks. It writes `mikai-0.1.0-source.tar.gz`, `SHA256SUMS`, and `source-provenance.json`. Provenance lists every included path and SHA-256, the current `HEAD`, worktree dirty status, and a deterministic tree digest. The archive is reproducible for identical file contents and modes. For each archive member, the per-file SHA-256 in provenance is computed from the exact bytes consumed to write that member; the tree digest is derived from those per-file hashes. Packaging fails if the file does not supply the size recorded when opened, if an extra byte remains after that size, or if its size differs after reading. A same-size edit made after file selection but before reading is represented by the bytes actually archived, not by an earlier pre-read hash. This is not an atomic filesystem snapshot: concurrent edits can make different files reflect different moments, and edits after a member has been read are not detected. Avoid changing the worktree during packaging.

This is a **local worktree snapshot**, not a commit or tag: intended staged, modified, and untracked files can be present, while deleted paths are absent. It makes no claim that the snapshot equals `HEAD` or any release tag. It does not stage or commit files. Review its file list and contents before sharing; keep private/unrelated material out of the worktree and heed the project's separate third-party source and redistribution obligations.

Trusted-tag CI source archives are a separate release path. The release workflow validates a version tag reachable from the default branch, checks out that exact commit, and binds release artifacts to it. For an actual trusted-tag preparation, use the documented CI workflow and its run evidence; do not substitute this local preview for its commit-bound archive or treat either path as authorization to publish.