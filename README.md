## What is this?
This is a modification of the QuakeSpasm source code that replaces the entirety of Quake's ancient software-based sound system (and the SDL implementation and pluggable codec system) with a completely new sound system based on [FMOD Core](https://www.fmod.com/core).

This brings Quake's audio to the modern age, with wide hardware and OS support, crystal clear sound quality, and adding features like 7.1 surround support basically for free.

## What's the point?
There isn't much point, this is just a fun "what-if" project. Having worked with FMOD Studio on a professional basis, I thought it would be an interesting challenge to see if I could rebuild Quake's audio system using FMOD while maintaining the authentic sound and feel of Quake, including all of its weird quirks. And I'm glad to say I succeeded in that. I've played Quake for over 25 years and to my veteran ears this FMOD implementation sounds indistinguishable from the original Quake. It sounds amazing in surround too.

I know in licensing terms this project is a bit of a dead end. FMOD does not conform to the GPLv2 license that Quake's source code was originally open sourced under, so publishing a precompiled build that is linked against the FMOD libraries is technically not allowed. Hence why this project is being published as source only, so you'll have to do the compiling and linking yourself if you want to try it.

## How do I use it?
You compile it just like you would standard QuakeSpasm. The project files contain a new "quakespasm-fmod" project, which will build a Quake executable with the right code and compiler flags, and link it against the FMOD libraries.

If you're copying the resulting executable to a different directory to play Quake, be sure to copy the FMOD libraries over as well (fmod and fmodL). You can check that FMOD is active by looking for a line like this in the console log:

    FMOD version 2.02.06, driver 'Default', Stereo speaker mode, 48000 Hz, 2 channels

If you have a surround sound setup correctly configured in your OS, then that should automatically be reflected in the speaker mode and channel count.

## What's next?
The nice thing about using FMOD is that it opens up a whole new world of possibilities for Quake's audio. You already get full surround sound support for free, but FMOD also includes a powerful DSP that offers a ton of modern audio features.

Focus in this initial version of QuakeSpasm-FMOD has been on authenticity, but it's entirely possible to craft a different, more realistic soundscape for Quake using advanced 3D reverb and mixing effects. It should even be possible to send entire map structures to FMOD and implement proper geometry-based occlusion!

I have no plans at the moment to make any of this myself, but it will remain in the back of my mind and perhaps someone else will feel inspired to really take Quake's audio to the next level someday.
