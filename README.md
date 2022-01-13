VapourSynth api3-to-api4 brdige
-------------------------------

# Introduction

Recent VapourSynth releases (R55+) introduced a new API version (api4) that is
not compatible with the previous plugin API (api3). Even though newer VS versions
can load plugins using either api3 or api4, older VS versions are stuck with
api3 plugins. This poses a dilemma for (video) plugin authors:
 - either you abandon users still stuck with older api3 VS versions by migrating to api4
 - or, you maintain two copies (branches) of essentially the same code.

The best course of action, IMHO, is to stick with api3 to support both worlds
without maintaining two copies of essentially the same code.

As anyone who has migrated video plugin from api3 to api4 would testify, there
aren't that many differences between the two APIs. One might ask: is it possible
to support both both API versions without duplicating code?

The answer is yes, and this project is one solution that should help with this
situation.

# Usage for End Users

For end users of VS plugins, this project can be used to load api4 *vdieo* plugins
in api3 VS. (api3 VS does not have audio support, so obviously this project won't
change that.)

Suppose you are stuck with VS R54, but you want to use a *video* plugin `filter.dll`
that only has api4 support. You can download the released `api3.dll` and save it
as `filter.api3.dll` along side with `filter.dll` into VS `plugins` directory. And
then you should be able to use the filter in your api3 VS as usual.

Note for Unix users: as this process involves making multiple copies of `api3.so`
if you have multiple api4 plugins to wrap, in order to save disk space, please
use *hard* links instead of symbolic links.

# Usage for Plugin Developers

For plugin developers, this project can help you support both api3 and api4 without
any code duplication. Just migrate to api4 and include api3.cc in your project. And
the rest should be automatically handled.

Examples:
 - [TCanny](https://github.com/HomeOfVapourSynthEvolution/VapourSynth-TCanny) has switched to api4 recently. By using this project, we just need to slightly [tweak](https://github.com/AmusementClub/VapourSynth-TCanny/commit/4700c10c0118a9178604240d3fe131bf72228e72) its build workflow to make a hybrid api3/api4 filter [release](https://github.com/AmusementClub/VapourSynth-TCanny/releases/tag/r13.AC2).
 - One [FFT3DFilter](https://github.com/myrsloik/VapourSynth-FFT3DFilter) fork has received some significant speed optimizations, but it's only available with an api4 interface. Again, we [tweaked](https://github.com/AmusementClub/VapourSynth-FFT3DFilter/commit/65a310689d29e7f4725b8169d3bac2c0f577367e) the build workflow to produce a hybrid api3/api4 filter [release](https://github.com/AmusementClub/VapourSynth-FFT3DFilter/releases/tag/R2.AC).
