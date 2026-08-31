# Compatibility Runtime Engine

This directory is reserved for the modernization work that turns ipaSim compatibility knowledge into a machine-readable, generated runtime surface rather than a growing collection of one-off symbol patches.

Implementation will proceed in small, testable PRs. The first milestone is a compatibility manifest generated from SDK metadata and current runtime providers. Unsupported behavior must remain explicit; generated metadata must never fabricate success.
