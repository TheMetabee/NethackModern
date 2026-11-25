Monocypher 4.0.2, for NetHack Modern's updater (win/share/fxupdate.c)
====================================================================

The game checks each update's minisign signature (Ed25519 over a
BLAKE2b-512 hash of the file) before installing it. These files do that
arithmetic; they touch nothing but memory.

Source: https://github.com/LoupVaillant/Monocypher, tag 4.0.2
        src/monocypher.c, src/monocypher.h,
        src/optional/monocypher-ed25519.c, src/optional/monocypher-ed25519.h,
        LICENCE.md
        (unchanged; files from the git tag say "version __git__" where
        the release tarball has the number)
Licence: BSD-2-Clause OR CC0-1.0, at your choice (LICENCE.md)

SHA-256 of the files as downloaded:
02174117935699d418443c75a558a287deb06ef8cf7c1adced61d9047d2f323d  monocypher.c
fcaf6ed771358bb4f40fba016f6518ae86ec02b1b877d2cc35ad92d3a26fd7b3  monocypher.h
97d581639dfa72be08a6d57deb7d79b736be001cb416819cab196d22559d242b  monocypher-ed25519.c
3a3035181f991a158d0e1c7567258f0bae8ba0f1f23c5512b4a1db1b3c9730ce  monocypher-ed25519.h
a5781770269d2516e52ba4863f790c10a16da4089a1e81823aee19ff1e9026b0  LICENCE.md
