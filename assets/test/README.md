# Test assets

This directory contains small, redistributable assets used to validate import, cooking, rendering, animation, lighting, and editor workflows. Test assets are not engine branding or production game content.

Each imported asset set must keep its own source, license, upstream revision, and file hashes next to the files. Assets with unclear licensing are not copied from reference engines.

Tracked fixtures currently contain only the original three small static Kenney GLB files needed by the bootstrap editor. Public 2D, texture, material, and animation libraries are recorded as links in `docs/research/2026-public-test-asset-libraries.zh-CN.md`; they will be fetched into a content-addressed cache only when their importer milestones begin.

Large or redistribution-restricted files belong under `assets/local-test/`. Their binaries are ignored, while a tracked manifest records expected names and hashes so the same workstation can run optional importer tests.
