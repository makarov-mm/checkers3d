# 3D Checkers — C++23, Win32 and OpenGL 4.6

[![GitHub Sponsors](https://img.shields.io/github/sponsors/makarov-mm?style=flat&logo=github)](https://github.com/sponsors/makarov-mm)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)
![OpenGL](https://img.shields.io/badge/OpenGL-4.6-5586A4?logo=opengl&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-0078D6?logo=windows&logoColor=white)
[![LinkedIn](https://img.shields.io/badge/LinkedIn-0A66C2?logo=linkedin&logoColor=white)](https://www.linkedin.com/in/makarov-mm/)
[![Threads](https://img.shields.io/badge/Threads-000000?logo=threads&logoColor=white)](https://www.threads.net/@m.m.makarov)
[![Instagram](https://img.shields.io/badge/Instagram-E4405F?logo=instagram&logoColor=white)](https://www.instagram.com/m.m.makarov/)

A 3D draughts game for Windows written in modern C++ on plain Win32 and OpenGL 4.6, with Russian draughts, English draughts (checkers) and giveaway.

There is no game engine, framework or third-party library, and no model, texture or sound file: the pieces are turned on a lathe in code at startup, the materials are procedural, the lighting environment is baked on the GPU, and the sounds are synthesized. The shaders are built into the executable, so the game is a single `.exe`.

![Screenshot](screenshot.png)

![Themes](themes.png)

## Features

### Rules

- **Russian draughts** (the default): men move forward and take forwards and backwards, kings fly along the diagonals, a man crowned in the middle of a capture goes on taking as a king, White begins
- **English draughts**: men move and take forwards only, kings move one square, a crowning ends the move, Black begins
- **Giveaway** on the Russian rules: the side that loses all its pieces, or is left without a move, wins
- Taking is compulsory; a capture goes on while anything can be taken; a piece taken stays on the board until the capture is over and cannot be jumped twice (the "Turkish strike")
- Draws by threefold repetition, by 15 moves (40 in English draughts) made with kings only and nothing taken, and a king against a king

### Play

- A computer opponent with five levels, from Beginner to Master: iterative deepening, principal variation search, a transposition table, late move reductions, killer and history heuristics, and every capture followed to the end. It thinks in the background, so the window stays responsive
- Play White, play Black, two players on one computer, or watch the computer play itself
- Move by clicking or by dragging the pieces. The pieces that must take are marked; hovering over a square shows which pieces the move would take; when several captures end on the same square, the route is chosen square by square
- Undo and redo, the move list in Russian or numbered notation, the men and kings each side has left
- Hints: an arrow follows the whole route of the suggested move
- Copy the game as PDN or the position as a PDN FEN, and paste a PDN game or position to continue from it
- The game in progress and the settings are kept between sessions
- English and Russian interface, chosen from the system language
- Sound effects, full screen, per-monitor DPI awareness, a dark title bar
- No work while nothing moves: the game waits for input instead of drawing the same picture

### Graphics

- Physically based shading: GGX highlights, a clear lacquer coat, soft light inside the pieces, image-based lighting from a procedural studio environment
- Soft shadows (PCSS), contact shadows, and the pieces reflected in the polished board
- HDR rendering with 4× MSAA, bloom, ACES tone mapping, vignette and dithering
- Smooth edges everywhere: multisampling for the outlines, and the edges of the squares, the joints and the lines drawn by the shaders filtered over each pixel; highlights widen where the surface curves within a pixel, so they do not sparkle
- Pieces turned on a lathe, with grooves round the side and rings on top; a king is two pieces stacked, with a crown inlaid on top
- Animation: the pieces glide and hop over the pieces they take, the pieces taken glow and fly to neat piles beside the board, the crown drops into place, and a new game dissolves in
- Three themes: Walnut, Marble and Neon
- An interface of frosted-glass panels, sharp at any screen scale

## Controls

| Action | Control |
|---|---|
| Move a piece | Click it, then where it goes — or drag it |
| Choose a capture route | Click the squares on the way; `Esc` steps back |
| Turn the view | Right drag, or drag beside the pieces |
| Zoom | Mouse wheel |
| Take back / replay a move | `Ctrl+Z` / `Ctrl+Y`, `←` / `→`, `Backspace` |
| Hint | `H` |
| New game | `N` |
| Flip the board | `F` |
| View from above | `V` |
| Reset the view | `Home` |
| Computer level | `1` … `5` |
| Who plays whom | `M` |
| Rules: Russian, English, giveaway | `R` (`Shift+R` back) |
| Next theme | `T` (`Shift+T` back) |
| Sound on / off | `S` |
| English / Russian | `L` |
| Show / hide the panel | `Tab` |
| Full screen | `F11`, `Alt+Enter` |
| Copy the game (PDN) / position (FEN) | `Ctrl+C` / `Ctrl+Shift+C` |
| Paste a PDN game or position | `Ctrl+V` |
| Help | `F1` |
| Leave full screen, deselect | `Esc` |

## Requirements

- Windows 10 (version 1703 or later) or Windows 11, x64
- A graphics card and driver with OpenGL 4.6 (4.5 is enough)
- Visual Studio 2026 with the **Desktop development with C++** workload; the project uses the C++23 language standard (`/std:c++23`, or `/std:c++23preview` with MSVC Build Tools older than 14.52)

## Building and running

Open `Checkers3D.slnx` in Visual Studio, choose the **Release | x64** configuration and run the project (`Ctrl+F5`).

From a Developer PowerShell:

```powershell
msbuild Checkers3D.slnx -p:Configuration=Release -p:Platform=x64
.\bin\x64\Release\Checkers3D.exe
```

The Release build links the C++ runtime statically, so `Checkers3D.exe` runs on its own. The settings and the game in progress are kept in `%APPDATA%\Checkers3D\Checkers3D.ini`.

## Project structure

```text
.
├── assets/                Checkers3D.ico
├── shaders/               GLSL shaders, built into the executable as resources
├── src/
│   ├── Main.cpp           entry point
│   ├── App.*              the window, game flow, input and interface
│   ├── Draughts.*         rules of the three variants: move generation, notation, PDN, the game record
│   ├── Engine.*           the computer player: search and evaluation
│   ├── PieceModels.*      the piece turned on a lathe
│   ├── BoardGeometry.*    meshes of the board, its frame, the table and the hint arrow
│   ├── BoardView.*        the pieces on the board, their animation and the piles of pieces taken
│   ├── Camera.*           orbit camera and mouse rays
│   ├── Renderer.*         the rendering pipeline
│   ├── Themes.*           materials and lighting of the themes
│   ├── Gl.*               OpenGL context (WGL) and functions
│   ├── Text.*             font atlases rendered with GDI
│   ├── UiPainter.*        drawing of the interface
│   ├── Sound.*            synthesized sound effects
│   ├── Settings.*         the settings file
│   ├── Strings.*          English and Russian texts
│   ├── Resources.*        access to the resources built into the executable
│   ├── MathTypes.h        vectors and matrices
│   └── Win32.h            windows.h and UTF-8 conversions
├── Checkers3D.rc          the icon, version information and shaders
├── Checkers3D.vcxproj
└── Checkers3D.slnx
```

## How a frame is drawn

1. **Shadow map** — the pieces seen from the key light, for soft shadows.
2. **Reflection** — the pieces mirrored under the board, drawn at half resolution and blurred into mipmaps, so the polish of each theme can show a sharper or softer reflection.
3. **Main pass** — the table, the board and the pieces in HDR with 4× MSAA, lit by the key light and the baked environment; the background is the environment itself.
4. **Post-processing** — the samples are resolved, bright parts spread into a six-level bloom, and the composite pass applies tone mapping and vignetting and frosts the glass behind the interface panels.
5. **Interface** — panels, text and icons, drawn on top.

The environment maps are baked with compute shaders when the theme changes, and the BRDF table once at startup.

## License

MIT License

Copyright (c) 2026 Mykhailo Makarov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Support

If you found this project interesting or useful, you can support my work:

[![GitHub Sponsors](https://img.shields.io/github/sponsors/makarov-mm?style=flat&logo=github)](https://github.com/sponsors/makarov-mm)
