# Local test assets

This directory contains optional source assets that can be used on this workstation but must not be redistributed by the public Noemancer repository.

Binary FBX files are intentionally ignored by Git. Tracked manifests record filenames, byte sizes, hashes, provenance, and intended coverage. A local asset is available only when its SHA-256 matches the manifest; missing local assets must produce a skipped optional test rather than a failed public build.

Do not move a local asset into `assets/test/` until its license explicitly permits redistribution of the source file.
