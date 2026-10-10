# Game data, derived assets and provenance

Supply supported game data from your own lawful source. The public fork and release packages must not include proprietary maps, textures, audio, movies, complete game executables, console dumps/keys, or Nintendo SDK components. Keep these outside the checkout. Owning a game does not grant a general right to redistribute its contents.

The host importer must identify the exact supported source revision, validate inputs, preserve source files unchanged and generate deterministic versioned Wii caches with a manifest. Its baseline, covering inventory, textures, recorded animations and sound containers, is the [content pipeline](CONTENT-PIPELINE.md); other content categories are still planned work. Do not advertise an arbitrary Xbox disc dump as accepted until tested. Conversion may change representation; it does not authorize skipping movies, encounters, geometry, animations or other required content.

Debug fixtures should be authored/synthetic or otherwise redistributable with clear provenance. Reference-game decompilations are study material, not permission to copy code without inspecting notices and provenance. Do not import proprietary SDK implementations from a matching-decomp tree. A game dump is not needed to use libogc's ordinary controller APIs.

The inspected upstream [LICENSE.md](https://github.com/OpenCommunityEdition/OpenCE/blob/2b0327bc80ca38c90894cb56b19652cbe85733ff/LICENSE.md) contains CC0. Preserve that file and all third-party/per-file notices. It is not a blanket license over game assets, trademarks or every external component. Review provenance before public code reuse and packaging.

Local logs should use data fingerprints rather than absolute dump paths. Saves belong to a versioned project directory with recoverable writes and migrations. Never overwrite a user's only valid state or place their source data in CI artifacts.

The release allowlist is executable, public instructions, authored metadata/icon, appropriate notices, build manifest and sanitized evidence. Audit the final archive; do not assume .gitignore controls what an archiver packages.
