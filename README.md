# Sketch

Drawing and annotation for OBS Studio 33+, by Voidscape Development.

Sketch adds a **Sketch Canvas** source and a **Sketch** filter, plus a **Sketch** dock where you draw on a live view of the
target. The dock pops out into its own window (tools move into the top bar) and docks back with one click. You can also
draw from a phone or tablet in any browser on your network.

## Features

- **Targets:** a standalone *Sketch Canvas* source (transparent, whiteboard, chalkboard or grid background) or the
  *Sketch* filter on any source (draw over a webcam, game capture, etc.).
- **Tools:** pen, brush, highlighter, eraser, line, arrow, rectangle, ellipse, text, plus stamps, fill, blur, pixelate,
  laser pointer and image under **More tools**.
- **Brush options:** color swatches and custom colors, size, opacity, smoothing, pen pressure (tablet eraser end is
  detected), solid / dashed / glow styles, filled shapes, and strokes that fade out after a delay.
- **Canvas:** undo/redo, clear, import an image, export PNG. Drawings are saved with the scene collection (optional) and
  can clear automatically on scene switch.
- **Remote drawing:** enable it in the dock's settings and open the shown URL on a phone or tablet on the same network.
  The page shows a live snapshot of the target and supports pen pressure. Access needs the token in the URL.
- **Hotkeys:** every tool, undo, redo and clear are assignable in *Settings → Hotkeys*.
- Uses standard OBS widgets, so it follows the active OBS theme.

## Usage

1. Add a **Sketch Canvas** source to a scene, or add the **Sketch** filter to an existing source.
2. Open **Docks → Sketch**, pick the target in the dropdown, and draw.
3. Use the pop-out button to move the panel into its own window; close the window or press the dock button to return it.

Notes:

- Blur and pixelate on the **filter** affect the source underneath; on the **Sketch Canvas** source they affect the
  drawing itself.
- Remote drawing uses plain HTTP on your LAN (default port 4477). Keep the URL private; anyone with it can draw.

## Building

Built with the standard [OBS plugin template](https://github.com/obsproject/obs-plugintemplate) tooling. Requires Qt 6
(Core, Widgets, Network, Svg) and obs-frontend-api; the dependency versions are pinned in `buildspec.json`.

```sh
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
