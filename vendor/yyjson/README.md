# yyjson

`upstream/` is the pinned yyjson 0.12.0 Git submodule from
<https://github.com/ibireme/yyjson>. Initialize it with:

```sh
git submodule update --init vendor/yyjson/upstream
```

`CMakeLists.txt` compiles the unmodified upstream `src/yyjson.c` twice, once for
each private consumer. The resulting objects are absorbed into cog-provider and
cog-toolkit. Their external symbols retain the `cog_provider_` and `cog_toolkit_`
prefixes; consumers need no public yyjson target or headers. Sanitizer compile
flags follow the owning Cog build. The upstream license is installed with each
archive's notices.

`provider_prefix.h` and `toolkit_prefix.h` rename external API declarations using
forced inclusion when compiling the upstream source. Private implementation
headers include the corresponding map before upstream `yyjson.h`. Inline
functions and private types keep their upstream spelling. Regenerate/check the
maps from the pinned upstream header with:

```sh
python3 vendor/yyjson/generate_prefix.py
python3 vendor/yyjson/generate_prefix.py --check
```

When updating the submodule, regenerate both maps, run provider/toolkit and
integration tests, and check the archives with `nm` for unprefixed external
`yyjson_`/`unsafe_yyjson_` definitions. Standalone `pocs/` retain their reference
sources; production builds do not link them.
