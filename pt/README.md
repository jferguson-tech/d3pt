# pt

Engine-independent path tracing core, MIT licensed (see `LICENSE`).

Nothing in this directory includes or is derived from the Quake 2 sources.
The engine talks to it only through `include/pt.h`; the GPL glue that adapts
Quake 2's renderer interface to that header lives in `../ref_pt`.

- `include/pt.h` - the C API
- `cpu/` - CPU backend
- `rtx/` - Vulkan backend (Nvidia only for now): the same tracer as compute
  shaders using ray queries; it builds its light lists with `cpu/pt_world.cpp`
- `water/` - shallow water simulation for bodies of liquid
- `material/` - makes a normal, roughness and metal map from the colours of a
  hand painted texture, reading its painted highlights and shadows as shape
  and telling metal from what covers it by colour, gives the texture back
  with that painted light taken out, works out what a metal painted that
  dark reflects, and tells the lit part of a lamp's or a screen's picture
  from its frame
- `png/` - PNG writer
