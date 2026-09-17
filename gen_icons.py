#!/usr/bin/env python3
"""
Generate PS Vita button icon PNGs for VitaCam gallery UI.
Each icon is 32x32 RGBA PNG.
"""
import os
import math
from PIL import Image, ImageDraw

OUT_DIR = "sce_sys/icons"
os.makedirs(OUT_DIR, exist_ok=True)

SIZE = 32
BG_COLOR = (20, 26, 48, 255)       # Dark navy
BG_ALPHA = (0, 0, 0, 0)            # Transparent
WHITE = (255, 255, 255, 255)
RED   = (220, 60, 60, 255)
BLUE  = (0, 180, 255, 255)

def make_circle_bg(size, color):
    """Create a circle background."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse([1, 1, size-2, size-2], fill=color)
    return img, d

def draw_thick_circle_ring(d, cx, cy, r, thickness, color):
    """Draw a thick ring using multiple ellipses."""
    for t in range(thickness):
        ri = r - t
        if ri > 0:
            d.ellipse([cx-ri, cy-ri, cx+ri, cy+ri], outline=color, width=1)

# ============================================================
# Cross (X) button - blue circle, white X
# ============================================================
img, d = make_circle_bg(SIZE, BG_COLOR)
m = 8  # margin from edge of circle
d.line([(m, m), (SIZE-m, SIZE-m)], fill=WHITE, width=5)
d.line([(SIZE-m, m), (m, SIZE-m)], fill=WHITE, width=5)
img.save(f"{OUT_DIR}/btn_cross.png")
print(f"Saved btn_cross.png")

# ============================================================
# Circle (O) button - blue circle, white ring
# ============================================================
img, d = make_circle_bg(SIZE, BG_COLOR)
cx, cy = SIZE//2, SIZE//2
r = 9
draw_thick_circle_ring(d, cx, cy, r, 3, WHITE)
img.save(f"{OUT_DIR}/btn_circle.png")
print(f"Saved btn_circle.png")

# ============================================================
# Triangle button - blue circle, white triangle outline
# ============================================================
img, d = make_circle_bg(SIZE, BG_COLOR)
m = 6
pts = [(SIZE//2, m), (m, SIZE-m), (SIZE-m, SIZE-m)]
d.polygon(pts, outline=WHITE, fill=None)
# Thicken: draw it 3 times slightly offset
for dx, dy in [(0,0),(0,1),(1,0)]:
    p2 = [(x+dx, y+dy) for x,y in pts]
    d.polygon(p2, outline=WHITE, fill=None)
img.save(f"{OUT_DIR}/btn_triangle.png")
print(f"Saved btn_triangle.png")

# ============================================================
# Trash / Delete icon - red rectangle, white trash symbol
# ============================================================
img = Image.new("RGBA", (SIZE, SIZE), (0,0,0,0))
d = ImageDraw.Draw(img)
# Red rounded rect background
d.rounded_rectangle([1, 1, SIZE-2, SIZE-2], radius=5, fill=(180, 40, 40, 240))
# Trash can body
d.rectangle([9, 14, 23, 26], outline=WHITE, width=2)
# Lid
d.line([(7, 13), (25, 13)], fill=WHITE, width=2)
# Handle
d.rectangle([13, 10, 19, 14], outline=WHITE, width=1)
# Lines inside
d.line([(13, 16), (13, 24)], fill=WHITE, width=1)
d.line([(16, 16), (16, 24)], fill=WHITE, width=1)
d.line([(19, 16), (19, 24)], fill=WHITE, width=1)
img.save(f"{OUT_DIR}/btn_trash.png")
print(f"Saved btn_trash.png")

# ============================================================
# Back arrow icon - dark rounded rect, white left arrow
# ============================================================
img = Image.new("RGBA", (SIZE, SIZE), (0,0,0,0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([1, 1, SIZE-2, SIZE-2], radius=5, fill=(32, 40, 60, 230))
# Arrow: < shape
cx, cy = SIZE//2, SIZE//2
aw = 8
d.line([(cx+4, cy-aw), (cx-4, cy), (cx+4, cy+aw)], fill=WHITE, width=3)
img.save(f"{OUT_DIR}/btn_back.png")
print(f"Saved btn_back.png")

# ============================================================
# Checkmark icon - green rounded rect, white checkmark
# ============================================================
img = Image.new("RGBA", (SIZE, SIZE), (0,0,0,0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([1, 1, SIZE-2, SIZE-2], radius=5, fill=(0, 180, 100, 240))
# Checkmark
d.line([(8, 16), (13, 22), (24, 10)], fill=WHITE, width=3)
img.save(f"{OUT_DIR}/btn_check.png")
print(f"Saved btn_check.png")

# ============================================================
# Select mode icon - dark rounded rect, white dashed square
# ============================================================
img = Image.new("RGBA", (SIZE, SIZE), (0,0,0,0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([1, 1, SIZE-2, SIZE-2], radius=5, fill=(40, 50, 80, 230))
# Dashed selection box
for i in range(0, 16, 4):
    d.line([(8+i, 8), (8+i+2, 8)], fill=WHITE, width=2)          # top
    d.line([(8+i, 24), (8+i+2, 24)], fill=WHITE, width=2)         # bottom
    d.line([(8, 8+i), (8, 8+i+2)], fill=WHITE, width=2)           # left
    d.line([(24, 8+i), (24, 8+i+2)], fill=WHITE, width=2)         # right
img.save(f"{OUT_DIR}/btn_select.png")
print(f"Saved btn_select.png")

# ============================================================
# Camera icon (back to camera)
# ============================================================
img = Image.new("RGBA", (SIZE, SIZE), (0,0,0,0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([1, 1, SIZE-2, SIZE-2], radius=5, fill=(20, 26, 48, 230))
# Camera body
d.rounded_rectangle([5, 12, 27, 24], radius=2, outline=WHITE, width=2)
# Lens
d.ellipse([12, 14, 20, 22], outline=WHITE, width=2)
# Viewfinder notch
d.rectangle([13, 9, 19, 13], fill=WHITE)
img.save(f"{OUT_DIR}/btn_camera.png")
print(f"Saved btn_camera.png")

# ============================================================
# Left arrow (navigate)
# ============================================================
img = Image.new("RGBA", (SIZE, SIZE), (0,0,0,0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([1, 1, SIZE-2, SIZE-2], radius=5, fill=(32, 40, 60, 200))
cx, cy = SIZE//2, SIZE//2
d.polygon([(cx+5, cy-8), (cx-5, cy), (cx+5, cy+8)], fill=WHITE)
img.save(f"{OUT_DIR}/btn_left.png")
print(f"Saved btn_left.png")

# ============================================================
# Right arrow (navigate)
# ============================================================
img = Image.new("RGBA", (SIZE, SIZE), (0,0,0,0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([1, 1, SIZE-2, SIZE-2], radius=5, fill=(32, 40, 60, 200))
cx, cy = SIZE//2, SIZE//2
d.polygon([(cx-5, cy-8), (cx+5, cy), (cx-5, cy+8)], fill=WHITE)
img.save(f"{OUT_DIR}/btn_right.png")
print(f"Saved btn_right.png")

print("\nAll icons generated successfully!")
print(f"Output directory: {OUT_DIR}/")
