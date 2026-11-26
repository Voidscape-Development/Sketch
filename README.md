# Sketch

Drawing and annotation for OBS Studio 33+, by Voidscape Development.

Sketch adds a **Sketch Canvas** source and a **Sketch** filter, plus a **Sketch** dock where you draw on a live view of the
target. The dock pops out into its own window (tools move into the top bar) and docks back with one click. You can also
draw from a phone or tablet in any browser on your network.

## Features

- **Targets:** a standalone *Sketch Canvas* source (transparent, whiteboard, chalkboard or grid background) or the
  *Sketch* filter on any source (draw over a webcam, game capture, etc.).
- **Tools:** pen, brush, highlighter, eraser, line, arrow, rectangle, ellipse, text, plus stamps, fill, blur, pixelate,
  laser pointer, image, spotlight, step markers and subject tracking under **More tools**.
- **Spotlight:** dims everything except a soft circle that follows the pointer while you hold it (size sets the radius,
  opacity the dimming).
- **Step markers:** each click drops the next numbered badge (1, 2, 3…). Undo takes the number back; Clear restarts at 1.
- **Subject tracking (filter only):** drag a box around something in the video and the ring follows it as it moves
  (template matching on a downscaled copy of the frame). Tracked rings are saved with the scene and removed by undo or
  Clear.
- **Ruler and grid:** the Ruler option (or holding Shift in the dock) draws straight lines at 15° steps and square
  boxes. *Snap shapes to grid* puts shape corners, stamps and markers on the 40 px grid.
- **Brush options:** color swatches and custom colors, size, opacity, smoothing, pen pressure (tablet eraser end is
  detected), solid / dashed / glow styles, filled shapes, and strokes that fade out after a delay.
- **Brush presets:** save the current tool and brush settings under a name and switch back from the preset menu
  next to the colors. Comes with Fineliner, Marker, Highlight, Neon, Dashed and Laser; presets 1–9 have hotkeys.
- **Canvas:** undo/redo, clear, import an image, export PNG. Drawings are saved with the scene collection (optional) and
  can clear automatically on scene switch.
- **Remote drawing:** enable it in the dock's settings and scan the QR code (or open the URL) on a phone or tablet on
  the same network. The page shows a live snapshot of the target, supports pen pressure, has every tool except Image,
  your brush presets, and the opacity/style/stamp/fill/ruler options. Pinch with two fingers to zoom and pan (snapshots
  get sharper while zoomed). It can be added to the home screen as an app.
- **Guests:** each guest picks a name, shown as a colored tag on stream while they draw (can be turned off). From the
  guests menu you can require approval for new guests, lock drawing for everyone, allow or block individual guests,
  disconnect them, and copy a view-only link. Guest undo, redo and clear only affect that guest's own strokes.
- **Hotkeys:** every tool, presets 1–9, undo, redo and clear are assignable in *Settings → Hotkeys*.
- Uses standard OBS widgets, so it follows the active OBS theme.

## Usage

1. Add a **Sketch Canvas** source to a scene, or add the **Sketch** filter to an existing source.
2. Open **Docks → Sketch**, pick the target in the dropdown, and draw.
3. Use the pop-out button to move the panel into its own window; close the window or press the dock button to return it.

Notes:

- Blur and pixelate on the **filter** affect the source underneath; on the **Sketch Canvas** source they affect the
  drawing itself.
- Remote drawing uses plain HTTP on your LAN (default port 4477). Keep the URL private; anyone with it can draw. Because
  it is not HTTPS, Android adds it to the home screen as a shortcut rather than an installed app.
- Tracking works best on subjects with texture or contrast. If it loses the subject, the ring stays put until the
  subject is found again nearby.

## Building

Built with the standard [OBS plugin template](https://github.com/obsproject/obs-plugintemplate) tooling. Requires Qt 6
(Core, Widgets, Network, Svg) and obs-frontend-api; the dependency versions are pinned in `buildspec.json`.

```sh
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Third-party code

[QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library) by Project Nayuki (MIT), in
`deps/qrcodegen`.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
