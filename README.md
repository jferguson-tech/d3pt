# d3pt

Doom 3 with a path traced renderer. Work in progress.

`r_pathTrace 1` draws a map's 3D view with a path tracer, on a graphics card
with ray tracing, through Vulkan. The game's own OpenGL renderer still draws
everything 2D over it, and is one console command away: `r_pathTrace 0`.

This is a modified version of the Doom 3 source code that id Software
released under the GPL on November 22, 2011. It is not the original, and it
is not made, endorsed or supported by id Software, ZeniMax Media or Bethesda.
id's own notes are in [README.txt](README.txt).

## The path tracer

What is traced:

- The map, and every model in it: monsters and other animated models, the
  weapon in hand, doors, lifts and whatever else moves.
- The game's lights, each as the box or cone the game fills with light, in
  the colour it has that frame: lights that flicker, fade, are switched on or
  move, and `testLight`. Lights the game casts no shadows from cast none
  here.
- Light that has bounced. This is the difference from the game's own
  picture: a room is lit by its walls as well as its lamps, and shadows are
  not black.
- Materials: the diffuse, bump and specular maps. A surface with a specular
  map shines as the game makes it shine, and reflects the room as well.

What is not, yet:

- Particles, glass and everything else see-through, decals, flares, and the
  screens of in-game computers, which are plain white.
- The sky, which is white through a window. Fog. Mirrors and cameras.
- The pictures the game's lights project: a light is an even box or cone.
- The player's own shadow.

It has been tried in a few places on the first three maps of the campaign,
on one card (a GeForce RTX 4090, about 70 frames a second at 1920x1080).
Everything else is untested, the flashlight and muzzle flashes included.
Expect places that are too bright or too dark beside the original, bumps
that look softer than the game's, and a blur behind things that move.

### Settings

The main ones are in the menus: Options has a Path Tracing item under
Controls, Game Options, System and Defaults, and in a game F10 (if it is not
bound to anything) or the `pathTraceMenu` command puts the same panel over
the view. Its Quality row sets scale, paths, bounces and lights weighed
together. F11 (likewise if it is not bound) goes between the path tracer and OpenGL.

All are console variables as well; most are saved.

| | |
|---|---|
| `r_pathTrace` | 1 draws 3D views with the path tracer |
| `r_ptScale` | resolution traced at, as a fraction of the view (1); lower is faster and softer |
| `r_ptAntialias` | 1 smooths edges by blending frames over time, and softens the whole picture with them (0) |
| `r_ptSamples`, `r_ptBounces` | paths a pixel a frame (1), bounces (3) |
| `r_ptLightScale` | how bright the game's lights are (1) |
| `r_ptSpecular`, `r_ptGloss` | how bright and how wide a highlight is (1.5, 0.5) |
| `r_ptMaterials` | 0 reads only a material's colour |
| `r_ptExposure`, `r_ptTonemap`, `r_ptContrast`, `r_ptSaturation` | the picture |
| `r_ptDenoise` | passes of the filter, 0 to 4 |
| `r_ptShadowFaces` | 1 casts shadows as the game does, from the side of a surface that faces the light |
| `r_ptEntities` | 0 traces the map alone |
| `com_maxFPS` | the most frames a second (144); the game still runs 60 times a second, and between two of those the view goes on turning and moving; 0 is a frame for each of the 60, as the game was |
| `r_lerpWorld` | 1 draws what the game moved in its last step on its way there, each frame: doors, lifts, monsters, lights (1); 0 steps them 60 times a second |
| `r_ptView` | draws the scene some other way: 4 light alone, 7 base colour, 8 normals |

Commands: `reloadPathTrace` hands the map to the tracer again, which
happens by itself when a setting that needs it is changed;
`listPathTraceLights` and `listPathTraceEntities` say what it has near you.

### How it is put together

The path tracer itself is the library in `pt/`. It knows nothing of Doom 3:
it is handed triangles, materials and lights, and draws a picture. It began
in [q2pt](https://github.com/jferguson-tech/q2pt), the same thing for
Quake 2, and is under the MIT license.

`neo/renderer/tr_pathtrace.cpp` stands between it and the game. It hands the
map over when a map's first view is drawn, and every frame the models of the
entities in the rooms joined to yours, the lights as they are then, and the
eye. The tracer draws into a picture it shares with OpenGL, which draws that
where the 3D view would have been.

## The game

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
and Ninja, and the Vulkan SDK, for the path tracer.

    build.bat

The results go to `run/`: `Doom3.exe`, `base/gamex64.dll` and
`d3xp/gamex64.dll`, with their `.pdb` files.

## Running

    cd run
    Doom3.exe +set r_pathTrace 1
    Doom3.exe +set fs_game d3xp

The path tracer needs a card and driver with Vulkan ray queries, and an
OpenGL driver that can open another API's picture
(`GL_EXT_memory_object_win32`). Without them the game says so in the console
and draws with OpenGL.

Any shape of screen: the view is as wide as the window or screen is, and
the menus and the HUD keep their 4:3 shape in the middle of it
(`r_guiAspect 0` stretches them). Options, System lists the sizes the screen
has and sets the one chosen at once. From the command line:

    Doom3.exe +set r_mode -2 +set r_fullscreen 1
    Doom3.exe +set r_mode -1 +set r_customWidth 2560 +set r_customHeight 1080 +set r_fullscreen 0

`r_mode -2` is the size of the desktop.

Doom 3 will not start while another copy of it is running.

## License

The path tracer in `pt/` is under the MIT license, see `pt/LICENSE`. The rest
of the source code is under the GNU General Public License version 3, with the
additional terms id Software attached to it. Both are in
[COPYING.txt](COPYING.txt). The game's data is not covered by it and belongs
to its owners.
