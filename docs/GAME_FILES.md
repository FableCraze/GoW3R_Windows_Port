# Game files

The project expects a complete, decrypted dump of God of War III Remastered
CUSA01623 update 1.02 obtained from the user's own copy. The directory selected in
the launcher is mounted as `/app0` without copying its assets.

The user must provide the following files locally:

- `eboot.bin`;
- `sce_sys/param.sfo`;
- `sce_module/libc.prx`;
- `sce_module/libSceFios2.prx`;
- all other directories and assets from the dump.

None of these files may be uploaded to the repository, issues, pull requests, CI
artifacts, or releases. This rule also applies to `GOW3IMG1` images, WAD, XVAG,
BSF, PKG, SELF, PRX, and any other files derived from game content.

The launcher validates the title ID, version, and known technical SHA-256 hash.
It does not decrypt PKG/SELF files, download firmware, or provide missing files.
