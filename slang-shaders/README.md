# Shaders

RetroArch Slang shader presets offered in **Settings → Graphics → Shader**. They run through
[librashader](https://github.com/SnowflakePowered/librashader) (`librashader.dll`, MPL-2.0 / GPL-3.0).

| Menu option | Preset | Author / licence |
|---|---|---|
| CRT-Royale | `crt/crt-royale.slangp` | TroggleMonkey, GPL-2.0-or-later (see `crt/shaders/crt-royale/LICENSE.TXT`) |
| CRT-Royale Fast | `crt/crt-royale-fake-bloom.slangp` | TroggleMonkey, GPL-2.0-or-later |
| CRT-Lottes | `crt/crt-lottes.slangp` | Timothy Lottes, public domain |

The files come from the libretro [slang-shaders](https://github.com/libretro/slang-shaders) collection.
The two CRT-Royale presets point their blur passes at `../blurs/blur9fast-*.slang` (an older layout of
the collection) instead of `../blurs/shaders/royale/`. Helper files in `include/` and `blurs/` keep their
original notices.
