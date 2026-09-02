# Phosphor Icons Provenance

Reviewed: 2026-09-02

## Component

| Field | Value |
| --- | --- |
| Upstream project | Phosphor Icons core assets |
| Upstream repository | <https://github.com/phosphor-icons/core> |
| Pinned release | `v2.0.8` |
| Source archive | <https://github.com/phosphor-icons/core/archive/refs/tags/v2.0.8.tar.gz> |
| Source archive SHA-256 | `8b4dd9afb96b50c92a613f5786af6a7dcff6e416442c42c12ed831cea050a698` |
| License | MIT |
| License file | `LICENSE` (upstream `core-2.0.8/LICENSE`, byte-identical) |
| License file SHA-256 | `b5b1f1da112d18ea2147decfd48ddc1bf2b5aeb6c265381579340e95b15a2bb2` |
| Modified | No. Every SVG below is byte-identical to its file in the pinned archive. |

The archive SHA-256 above was computed over the exact bytes downloaded from that URL on
2026-09-02 with `sha256sum`, not copied from an upstream publication.

## What is vendored, and what is not

ADR 0010 and `docs/ux/visual-language.md` require a curated, pinned subset rather than the
complete catalog: the upstream release carries 1248 icons in six weights (7488 files), and Bloom
embeds 43 of them in two weights (86 files, 360 KiB). `regular` is the default interface weight
and `fill` is the selected/toggled weight; the other four upstream weights (`thin`, `light`,
`bold`, `duotone`) are deliberately not vendored, because no implemented Bloom control uses them.

Bloom does not depend on a JavaScript, Node, npm, or web runtime to obtain or render these
assets, and nothing is fetched from a network while configuring, building, or launching Bloom.
The files are enumerated one by one in `src/ui/CMakeLists.txt` through `qt_add_resources` under
the Bloom-owned `/bloom/kit` resource prefix.

Upstream SVGs carry `fill="currentColor"`. Qt's `QSvgRenderer` does not resolve `currentColor`
(there is no such API through Qt 6.11), so Bloom resolves it from the applicable Qt palette role
at render time in `src/ui/kit/icons.cpp` -- a runtime substitution on a copy of the bytes, never
an edit to the vendored files.

## Vendored files

Digests are SHA-256 over the exact checked-in bytes.

### regular

| Path | SHA-256 |
| --- | --- |
| `regular/caret-down.svg` | `53f0cf2d0b144ac3cb07e353e0cd0853ffb3ed432d741460ce288edfcb0924b1` |
| `regular/caret-left.svg` | `623bf248fe21a8170344e168aaac2e174119cea5d7c5d0fb0dc84f581009475f` |
| `regular/caret-right.svg` | `e6dec01e074807965b7f3146b976a9b5e82c31d9c2c88aeb52e77cc2ab16c1d7` |
| `regular/caret-up.svg` | `bccc26004e73e5de783ac6dde607e3671e776261d81972ccdbd296b4aca21ca9` |
| `regular/check.svg` | `cbb89a8c42f283d4f846eb935e05d88c1be04462f16480e068c0f82b9ce40b12` |
| `regular/corners-in.svg` | `31e9060aafdd6e24f4b8aef7742a89e7e616e6e2da07160691d020cb5ab07dee` |
| `regular/corners-out.svg` | `fd79277404b3662b50b737127fb6bf6f63ae93c9b60d9f2aca9122a950ec08c8` |
| `regular/cube.svg` | `7c0f13cb492d0dd212e48ab259dabec4627df32d71aa00ce31b3228c0f35112b` |
| `regular/cursor.svg` | `f0ba62c40c27b39286904ad57ed71df3fbed8383b9fb4f00643db05a2d601bff` |
| `regular/diamond.svg` | `7f9ebf5bb51a955c248da77b9b8a50226a7ecd81706b59e78233d7ad2a9d9d45` |
| `regular/dots-three-vertical.svg` | `85fdecd2193efcd7e80a83607b66d2680fef8f3d1e93380c26f9851b24edbd01` |
| `regular/eye-slash.svg` | `070b0296a7df89ebf4cec12436e323b5531d458aa9aed3608924085889e1fd17` |
| `regular/eye.svg` | `91d39f20bf95e0f36c07bd79b83a85fe6ff4ca20e2759c4afee7419ef80403fe` |
| `regular/film-slate.svg` | `58b84e6c5a2950cfa0d8cae68c04e65dc32407dde463c3a2c79d265464726e0c` |
| `regular/film-strip.svg` | `6d079e87432258b9fe9eff9d10ababcfbca7a400537277a215360a436581987f` |
| `regular/folder.svg` | `4a882bcd4a91ae0a01887024f08d358932f183b6b171e87a3e48c5910364d507` |
| `regular/gear.svg` | `5c0cb3ba307c72babf78d952d9140c682ff30307fedc7fbb87c9a293c1e2fd64` |
| `regular/hand-grabbing.svg` | `5c07fc19f6101a86c10cfa3cb4685c388cd0b89a69aa46caa92e44b5ba599977` |
| `regular/hand.svg` | `b845c51acbe69c5ac2ec208c5648806f33c0a86b06d2dfa53ebe599ccb0f9c65` |
| `regular/image.svg` | `642a0575f7b5a7ea2f5fefa6f27a5dc57683911845b85213a7e33652c2a3179d` |
| `regular/info.svg` | `438a1f319d53d86e840af73fb7b1d224c9224a437b9fa449a4e2cb45a82a3552` |
| `regular/link-simple.svg` | `7a97dad4b2f16c3a386ea69ba78d29bf3a71c311f2e34100d79143559ba13151` |
| `regular/list.svg` | `fb1851842c74b40a3f42ce3d6ae0e67ede556b624218e79c0f4ead7d9ceec51f` |
| `regular/lock-simple-open.svg` | `55c3dcdb0db66b65f4afab5ead515b0db381aecfbac9f61ec1e2691d83cc5330` |
| `regular/lock-simple.svg` | `b99d3713ac86cb610eb66974d69de2aa1223c4f6387fb1053e3d375e62f5dc1c` |
| `regular/magnifying-glass.svg` | `f9fa2561978a9a3291e8ae0ebfcb651104f5069fade8f84871627bca68889cde` |
| `regular/minus.svg` | `5a068d1cfd707fde5ddb0ce0623958123b6a5afba98401318983aeb0fdad6382` |
| `regular/music-notes.svg` | `e40b3381d49b45275c8875d7e85f2332a11eb467f7ad9e7c9b4656c29a97e9e1` |
| `regular/pause.svg` | `7a9a8fe311234262b85c6e332f77db4ba9a59b6ca4292c41d327977cf61a2fc9` |
| `regular/play.svg` | `885b3ed2095889c4ff063dac35dc424cb9f6c00273ab44e2f90dc9dadbee661a` |
| `regular/plus.svg` | `d688feb9dd2e41c1721d6906a05a2a42e18cd774762fe706d72b2bdfd91cddc4` |
| `regular/repeat.svg` | `12cef5f8ba7074921f909838088154a6847b936b11342c28daa475c62b71df86` |
| `regular/skip-back.svg` | `20f318558e166d5dea09fb854a1bbc199e9891b17febed640e912a6ee4712d37` |
| `regular/skip-forward.svg` | `52f1ed5a89f1a38bf8537b38bd2da0501e396fc4cb03a2cdde38d248ceb03fbe` |
| `regular/speaker-simple-high.svg` | `3c0160ae73577763cf4304afc7766e8ed71f72bba3f6c1214a48931c0cba281b` |
| `regular/speaker-simple-slash.svg` | `cb7e3bd3c0255d0a22e4cc7eec9bd50c7898b8e87a44074aadd48accd815a916` |
| `regular/square-split-horizontal.svg` | `d37901731a53cc0edb101f8587af0a8122ad31d4c5b78c45453f1d6c1d26df70` |
| `regular/square-split-vertical.svg` | `5e720feffbac898b3ae728639b6ab663899aefb6d3e7efba4447afe22840b16c` |
| `regular/text-t.svg` | `00f5cc0ddab2aacaf377ffe956e2ee5375ec3395043d468cbb5937251df7fc69` |
| `regular/trash.svg` | `e6a830c0409f9e101e3695c981eef98427d39fd01f502b9cbd6900930b01f19c` |
| `regular/warning-circle.svg` | `e48a90760a68659fd3f06dd9187e471aead4877c72e915339071b2c7f7bd714c` |
| `regular/warning.svg` | `047c8edbdfddb9f1692bbf56acc85b7e4649b567c60eb39470e98e4f9ddc6a2e` |
| `regular/x.svg` | `7397b15e1fa66eaf6ff9b59fe91d483cb5c3bcf75b2fe052097ab6f0593b513a` |

### fill

| Path | SHA-256 |
| --- | --- |
| `fill/caret-down-fill.svg` | `33550432966b277d6cf891f4db186357f4a83b1f8a5c7770c6ad6f5dee9b0b34` |
| `fill/caret-left-fill.svg` | `a6300d93ee3016043fe02af787421093bedd301a0da992337d07b16404e65fa5` |
| `fill/caret-right-fill.svg` | `c62278a701f1e4c08f424ce4b5cac69c8adb23b496dae489d5545ea7e6c019ef` |
| `fill/caret-up-fill.svg` | `a4703a6e23011c86e9d7d18234b30bba36ba04f43f37ca08bb3d04beb9e3770b` |
| `fill/check-fill.svg` | `d114007dc371976dc3e8f4ebc59c5aef429fae59ae58d56acdf231a52b4700be` |
| `fill/corners-in-fill.svg` | `ade092d133654ec69ee3484ed10b79523ffe23eb9b8f9c96e5e1fe72b7af31be` |
| `fill/corners-out-fill.svg` | `206cdf1c6c7a3d2da17edebae2a122b5bcf8186f276170cac730a041c3077a1d` |
| `fill/cube-fill.svg` | `9c681bfc8547fbb142d5df4d69ad923af5c422fea1013e6d69c32cef9ced3de3` |
| `fill/cursor-fill.svg` | `b66c0ee5273847a03504f4c07b34eda3e8904a8fa9f607a1cc6c5527264c4a59` |
| `fill/diamond-fill.svg` | `e50d5227db3ca71ba862deb7a6e60b78eb27c7780ed2e2b856c31269b63f4a68` |
| `fill/dots-three-vertical-fill.svg` | `7dd0eff7a0107728f568f08679b5fefb8309534dff749f779fc21805e1b9186c` |
| `fill/eye-fill.svg` | `86381b51cb6f305019503b4c5d4b02fe8df5941bab3f1475ee873006a8708420` |
| `fill/eye-slash-fill.svg` | `c5c57b58e51b8b5eb30573a58fd1cc2672ef5d877759c01b948352ecb20b642c` |
| `fill/film-slate-fill.svg` | `9b93a0c8e74ddaa01ee402f7c182c0189e25df713eb533b86eb4b9ef08ac7aca` |
| `fill/film-strip-fill.svg` | `aae7abd682bd0985d015c90cd952e5fdb667c5c5e23a3494b30b422a6ab5ef5b` |
| `fill/folder-fill.svg` | `2217bd2f730884d7a8aec3e3358eb7500448b0e3d86f33a63142f40f0622bfe9` |
| `fill/gear-fill.svg` | `564f53f72b0bd4371dc14a3d39a3fcf4c5a1a74d285eb56fff465a954f993ec8` |
| `fill/hand-fill.svg` | `9a1598a0dce127935ad97ede769c77629f422de7a1eabe54b4ec1b2d0c19369b` |
| `fill/hand-grabbing-fill.svg` | `df9ace3ad62fe54f8f876b140c2824a7cf1ae55a087cd31649f3c562e465a39b` |
| `fill/image-fill.svg` | `8cbeb0cac6e7f683e08d8a71a5db21c10435c39d0487c3277b75b146a86abf42` |
| `fill/info-fill.svg` | `8240b810f7a3b076271a829dff61c7dd64257a781c30f29c938f88835f11136a` |
| `fill/link-simple-fill.svg` | `601ae01a732bdfcfa7a3e01dbe1302a32c4bd03b60d73c5c3a8f051793105e6a` |
| `fill/list-fill.svg` | `40af80e60bd65c8230d4e7aed12baa064a31b06d9d32a406ffdcf5f281ac99ba` |
| `fill/lock-simple-fill.svg` | `54b137caf94b8082e63a2831e5b726bbc9cd32ad40938bf5656ff4d44b442d95` |
| `fill/lock-simple-open-fill.svg` | `fa4603d9987e96952fb892c19a717bfa674af6520f1e469eb6a812411303c60e` |
| `fill/magnifying-glass-fill.svg` | `e7cf953bb787af2a4b11f8a248f35b56aab2f2b44f535d03593f490a4a633244` |
| `fill/minus-fill.svg` | `fc5b1fa0d71db88886b70da6a675f7de3e7d4007444e87ba68f174a9ecef037e` |
| `fill/music-notes-fill.svg` | `d8b530db5c510539e8235a5e80d7f984504f7b5ae24408d080a8468ae12e29dc` |
| `fill/pause-fill.svg` | `156d3cdfa5cea803caec50b35f49bc635070c8bd84e7fe78df94b025017ccee1` |
| `fill/play-fill.svg` | `6d2a75bc5700a68dec50516ebae121c307807e4e6fcca5c863e5b4af006df880` |
| `fill/plus-fill.svg` | `64ef1a2c9f693a531ddb970a6c6e7c9dcd9bf36a58afec66ecb57355449ce3d2` |
| `fill/repeat-fill.svg` | `0ccdc4397cd7c1e528c571d6ba3fb6764043d791b3de0ac86c6eaf5edb3a8ce6` |
| `fill/skip-back-fill.svg` | `65a2d664196ad28fff7288ed7a1870f508fdb922334ad1a49c54586dae7592d7` |
| `fill/skip-forward-fill.svg` | `2f13f5cbeab47627d30782e7942001e7f80f7da993df053d3be2b8c6cb737ae2` |
| `fill/speaker-simple-high-fill.svg` | `3414782909ef8fa9c30c54c9fa3325654282ce818336ec1f27393aca9074b365` |
| `fill/speaker-simple-slash-fill.svg` | `80439f069b5313a88af31cc8b07881cbd519f46ac3d5a38673bd6cb8c60c0cfd` |
| `fill/square-split-horizontal-fill.svg` | `cb047582904dc8ae92fe8f4ff9d0bb31df3c74a64bd5b0bc68eb6b2df3445f25` |
| `fill/square-split-vertical-fill.svg` | `0ace6381e25145a677d0e2841813c292436bd451d3c3c96def9061a82f4cb83b` |
| `fill/text-t-fill.svg` | `5ddee68d99ab4598641cf99fb7025cb18e2add9302ae4df31653418066ca0c6d` |
| `fill/trash-fill.svg` | `f78767cc15e1a7d6eea49c4efb515cf6fceaf07fbc421e8ce18373d07c14b673` |
| `fill/warning-circle-fill.svg` | `29199dd3ff20a7fc36380dc366c9004fafa98d979649dc2311c5b1f8fd968dac` |
| `fill/warning-fill.svg` | `90595f8a478ac1f156871ac8d8a35c7c4e372b8d101dad9aeb1bceadf9a2c7f0` |
| `fill/x-fill.svg` | `6ec0689770c1fb1dc7018039bef101079d2d20931c4661c3981cbe956f20c872` |

## Subset manifest digest

A single value covering the whole vendored subset, so a drift in any one asset is one comparison
away rather than 86:

```
SHA-256(sorted "<sha256>  <filename>\n" lines for every file above) = bf9a19c1a9f75236be13614c011668433909f284ecec518114bc17c48bc307e4
```

Reproduce with:

```
(cd regular && sha256sum *.svg; cd ../fill && sha256sum *.svg) | LC_ALL=C sort | sha256sum
```

## Status

Pinned and reviewed. A change to the icon set -- adding an icon, changing a weight, or moving to
a new upstream release -- replaces this record wholesale: new release row, new archive digest, new
file table, new manifest digest.
