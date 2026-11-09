# Lampa PS5

An experimental PS5 homebrew launcher based on [ProsperoWeb](https://github.com/kvaps/ProsperoWeb) (GPL-3.0-or-later).

- Home URL: `http://lampa.mx`
- Native title ID: `LAMP00001`
- Persistent profile: `/data/lampa-ps5`

## Automated build

Open **Actions → CI → Run workflow → main → Run workflow**, or push a commit to `main`.

Wait for the PS5 job to complete and download its `LAMP00001.zip` artifact. A cold WebKit build can take a long time and may require re-running to benefit from caching.

## Install

Requires a jailbreak-enabled PS5, ELF loader at port 9021, and ShadowMountPlus. Copy the extracted `LAMP00001/` directory to `/data/homebrew/LAMP00001/` and launch the title from the PS5 home screen.

This build is untested on console; confirm that Lampa loads and video playback works. The icon is still the upstream placeholder until replaced.
