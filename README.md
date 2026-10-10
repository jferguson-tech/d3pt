# d3pt

Doom 3 with a path traced renderer. Work in progress: so far this is the
game brought up to date, and the path tracer is not here yet.

This is a modified version of the Doom 3 source code that id Software
released under the GPL on November 22, 2011. It is not the original, and it
is not made, endorsed or supported by id Software, ZeniMax Media or Bethesda.
id's own notes are in [README.txt](README.txt).

## What works

The game builds as a 64-bit program with Visual Studio 2022 and runs the
campaign and Resurrection of Evil with the original OpenGL renderer.

Changed from the original release:

- CMake build for x64 in place of the Visual Studio 2010 projects.
- 64-bit fixes throughout: the memory allocator, the script interpreter and
  the game's event system, save games, demos, fonts, the cinematic decoder.
- The maths library runs on its generic C++ path. The MMX, SSE and 3DNow
  paths are inline assembly, which the 64-bit compiler does not take.
- A crash writes `crash.txt` beside the executable, with the call stack.

Left out of the build, though the sources are still in the tree:

- The level editor and the other tools written with MFC.
- OpenAL and EAX sound. The game uses its DirectSound path.
- curl, which multiplayer used to download files.

Not tested: multiplayer, the dedicated server, save games made by the 32-bit
game, and anything but Windows.

## Game data

You need your own copy of Doom 3 (the original game, not the BFG Edition),
patched to 1.3.1. No game data is in this repository.

Copy the `.pk4` files from the game's `base` folder to `run/base`, and for
Resurrection of Evil those from `d3xp` to `run/d3xp`.

## Building

Needs Visual Studio 2022 with the C++ desktop workload, which brings CMake
and Ninja.

    build.bat

The results go to `run/`: `Doom3.exe`, `base/gamex64.dll` and
`d3xp/gamex64.dll`, with their `.pdb` files. `build.bat Release` builds
without debug information.

## Running

    cd run
    Doom3.exe
    Doom3.exe +set fs_game d3xp

Doom 3 will not start while another copy of it is running.

## License

The source code is under the GNU General Public License version 3, with the
additional terms id Software attached to it. Both are in
[COPYING.txt](COPYING.txt). The game's data is not covered by it and belongs
to its owners.
