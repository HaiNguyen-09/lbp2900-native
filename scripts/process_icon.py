#!/usr/bin/env python3
import sys
import os
import gi
gi.require_version('GdkPixbuf', '2.0')
from gi.repository import GdkPixbuf

def process_icon(src_path, out_dir):
    if not os.path.isfile(src_path):
        print(f"Error: source icon '{src_path}' not found.", file=sys.stderr)
        return False

    print(f"Loading master icon from: {src_path}")
    src = GdkPixbuf.Pixbuf.new_from_file(src_path)
    w, h = src.get_width(), src.get_height()
    channels = src.get_n_channels()
    rowstride = src.get_rowstride()
    has_alpha = src.get_has_alpha()
    pixels = src.get_pixels()

    # Find bounding box of visible content
    if has_alpha:
        min_x, min_y, max_x, max_y = w, h, 0, 0
        for y in range(h):
            row_offset = y * rowstride
            for x in range(w):
                a = pixels[row_offset + x * channels + 3]
                if a > 8:
                    if x < min_x: min_x = x
                    if x > max_x: max_x = x
                    if y < min_y: min_y = y
                    if y > max_y: max_y = y
    else:
        min_x, min_y, max_x, max_y = 0, 0, w - 1, h - 1

    content_w = max(1, max_x - min_x + 1)
    content_h = max(1, max_y - min_y + 1)
    cx = (min_x + max_x) // 2
    cy = (min_y + max_y) // 2
    print(f"Detected icon content: {content_w}x{content_h} centered at ({cx}, {cy})")

    # Target content to occupy ~85% of square box for standard Linux desktop aesthetic
    box_size = int(max(content_w, content_h) / 0.86)
    box_size = min(box_size, max(w, h))

    half_box = box_size // 2
    box_x = max(0, min(cx - half_box, w - box_size))
    box_y = max(0, min(cy - half_box, h - box_size))

    if box_x + box_size > w:
        box_size = w - box_x
    if box_y + box_size > h:
        box_size = h - box_y

    sub = src.new_subpixbuf(box_x, box_y, box_size, box_size)

    sizes = [16, 24, 32, 48, 64, 128, 256, 512]
    for sz in sizes:
        d = os.path.join(out_dir, f"hicolor/{sz}x{sz}/apps")
        os.makedirs(d, exist_ok=True)
        scaled = sub.scale_simple(sz, sz, GdkPixbuf.InterpType.HYPER)
        target = os.path.join(d, "canon-lbp2900.png")
        scaled.savev(target, "png", [], [])
        print(f"  -> Generated {sz}x{sz} icon: {target}")

    pixmap_dir = os.path.join(out_dir, "pixmaps")
    os.makedirs(pixmap_dir, exist_ok=True)
    sub.scale_simple(256, 256, GdkPixbuf.InterpType.HYPER).savev(
        os.path.join(pixmap_dir, "canon-lbp2900.png"), "png", [], []
    )
    print("All multi-resolution icons processed successfully.")
    return True

if __name__ == "__main__":
    src_file = sys.argv[1] if len(sys.argv) > 1 else "/home/phoenix/Downloads/IMG_0664.PNG"
    if not os.path.isfile(src_file):
        src_file = "icons/canon-lbp2900-master.png"
    out_directory = sys.argv[2] if len(sys.argv) > 2 else "icons"
    success = process_icon(src_file, out_directory)
    sys.exit(0 if success else 1)
