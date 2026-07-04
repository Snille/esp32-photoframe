#!/usr/bin/env python3
"""Generate OOBE splash + setup-complete EPDGZ files for embedding in firmware.

The splash is composed with Pillow (works cross-platform incl. CI — no native
SVG renderer needed) in the "Forest (Dark)" look: pure-black background (an exact
e-paper palette colour, so it dithers perfectly clean), forest-green accents, and
a photo (Kakan) that gives every frame the same face. White QR slots are reserved
where the firmware draws its live WiFi / web-UI QR at runtime.

Geometry note: the image is composed in the board's *viewing* (mounting)
orientation, then rotated into the firmware's *native* panel buffer orientation
when they differ (see PANEL_ROTATION). The FireBeetle panel is natively 400x600
portrait but the frame is mounted landscape (600x400); the splash is embedded raw
and is NOT rotated by the server at runtime, so it must be pre-rotated here.

Dependencies:
  pip install qrcode pillow
  node + process-cli (npm ci in process-cli/)
"""

import argparse
import os
import shutil
import subprocess
import sys

try:
    import qrcode
except ImportError:
    print("Install qrcode: pip install qrcode")
    sys.exit(1)

try:
    from PIL import Image, ImageDraw, ImageEnhance, ImageFont
except ImportError:
    print("Install Pillow: pip install pillow")
    sys.exit(1)

# App download URL (kept for reference; the splash now reserves only the WiFi /
# web-UI QR slot that the firmware fills live).
APP_URL = "https://aitjcize.github.io/esp32-photoframe/#companion-app"

# Path to process-cli and the splash photo
PROCESS_CLI = os.path.join(os.path.dirname(__file__), "..", "process-cli", "cli.js")
CAT_IMAGE = os.path.join(os.path.dirname(__file__), "splash_assets", "cat.jpg")

# Forest (Dark) palette mapped to e-paper-friendly colours
BG = (0, 0, 0)  # pure black -> exact palette colour, no dither speckle
GREEN = (63, 174, 104)  # #3fae68 forest accent (maps to native green)
WHITE = (242, 242, 242)
MUTED = (150, 150, 150)

# Native panel buffer dims, taken from the e-paper drivers (NOT boards.json,
# which lists the *viewing* resolution for some boards and the *native* one for
# others). The splash is embedded raw and must match this exactly.
NATIVE_DIMS = {
    "dfrobot_firebeetle_esp32e": (400, 600),  # driver_ws4in_e6
    "dfrobot_firebeetle_esp32s3": (400, 600),  # driver_ws4in_e6 (same panel/HAT)
    "seeedstudio_xiao_ee02": (1200, 1600),  # driver_ed2208_nca
    "seeedstudio_xiao_ee04": (800, 480),  # driver_ed2208_gca
    "seeedstudio_reterminal_e1002": (800, 480),
    "waveshare_photopainter_73": (800, 480),
}

# Mounting orientation per board. The splash is baked at build time (one per
# board type), so a single default is chosen here; photos still rotate per unit
# via the server's display_rotation_deg. Default: landscape for every board.
# When a board's native panel orientation differs from its mounting, the splash
# is composed in the mounting (viewing) orientation then rotated into native.
# Rotation direction (90 vs 270) is verified on hardware at first flash.
MOUNT_LANDSCAPE = {b: True for b in NATIVE_DIMS}

# Rotation degree (display_rotation_deg) used to turn the viewing-orientation
# composition into the native buffer, for boards whose panel is mounted rotated.
# Direction is panel-specific (opposite handedness between panels) and verified
# on hardware: FireBeetle upright at 90, EE02 upright at 270 (90 was upside-down).
ROTATION_DEG = {
    "dfrobot_firebeetle_esp32e": 90,
    "dfrobot_firebeetle_esp32s3": 90,  # same panel/mounting as the ESP32-E FireBeetle
    "seeedstudio_xiao_ee02": 270,
}

# Screen sizes for the --size path (no board context = no rotation)
SCREEN_SIZES = {
    "800x480": (800, 480),
    "1200x1600": (1200, 1600),
}

# Per-screen text
TEXTS = {
    "splash": ("ESP Frame", "Scan for WiFi"),
    "setup_complete": ("All set!", "Scan for web UI"),
}


def _font(px, bold=True):
    names = (
        (
            "arialbd.ttf",
            "Arial Bold.ttf",
            "DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        )
        if bold
        else (
            "arial.ttf",
            "DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        )
    )
    for n in names:
        try:
            return ImageFont.truetype(n, int(px))
        except Exception:
            continue
    return ImageFont.load_default()


def _ctext(draw, text, cx, y, f, fill):
    b = draw.textbbox((0, 0), text, font=f)
    draw.text((cx - (b[2] - b[0]) / 2, y), text, font=f, fill=fill)


def _fit_font(draw, text, max_w, target_px, bold=True):
    """Largest font <= target_px whose text width fits in max_w."""
    px = target_px
    while px > 8:
        f = _font(px, bold=bold)
        if draw.textbbox((0, 0), text, font=f)[2] <= max_w:
            return f
        px *= 0.94
    return _font(px, bold=bold)


def _rounded_cat(size, radius):
    cat = Image.open(CAT_IMAGE).convert("RGB").resize((size, size), Image.LANCZOS)
    # Boost contrast + saturation a touch so the 6-colour dither reads cleaner.
    cat = ImageEnhance.Contrast(cat).enhance(1.12)
    cat = ImageEnhance.Color(cat).enhance(1.25)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, size - 1, size - 1], radius=radius, fill=255
    )
    return cat, mask


def compose(view_w, view_h, kind):
    """Compose the splash in the viewing orientation.

    Returns (PIL.Image, (qr_x, qr_y, qr_size), ip_anchor) where the QR rect is the
    white box the firmware fills with its live QR, and ip_anchor is
    (center_x, bottom_y, height) in *viewing* coords for the runtime IP line on
    the setup-complete screen (None for the OOBE splash). The IP text itself is
    NOT baked in — the firmware draws the live IP there via GUI_Paint.

    Unified layout (both screens): the title's top edge aligns with the cat's top
    edge, and the bottom element (QR on the OOBE splash, the IP line on
    setup-complete) has its bottom edge aligned with the cat's bottom edge — so
    the text column visually spans the same height as Kakan.
    """
    title, subtitle = TEXTS[kind]
    img = Image.new("RGB", (view_w, view_h), BG)
    d = ImageDraw.Draw(img)
    landscape = view_w >= view_h
    ip_anchor = None

    if landscape:
        # Cat on the left, title/subtitle/QR (+IP) stacked on the right.
        cs = int(view_h * 0.66)
        cx = int(view_w * 0.06)
        cy = (view_h - cs) // 2
        cat_top, cat_bot = cy, cy + cs
        rad = int(cs * 0.12)
        cat, mask = _rounded_cat(cs, rad)
        img.paste(cat, (cx, cy), mask)
        d.rounded_rectangle(
            [cx - 3, cy - 3, cx + cs + 3, cy + cs + 3],
            radius=rad + 3,
            outline=GREEN,
            width=3,
        )

        rx = cx + cs + int(view_w * 0.05)
        rcw = view_w - rx - int(view_w * 0.04)
        rcx = rx + rcw // 2

        # Title top-aligned with the cat's top edge.
        tf = _fit_font(d, title, rcw, view_h * 0.135)
        _ctext(d, title, rcx, cat_top, tf, WHITE)
        tb = d.textbbox((0, 0), title, font=tf)
        title_bot = cat_top + (tb[3] - tb[1]) + int(view_h * 0.02)

        # Green accent bar.
        bar_y = title_bot + int(view_h * 0.02)
        d.rounded_rectangle(
            [
                rcx - int(view_w * 0.06),
                bar_y,
                rcx + int(view_w * 0.06),
                bar_y + int(view_h * 0.02),
            ],
            radius=3,
            fill=GREEN,
        )

        # Subtitle.
        sf = _fit_font(d, subtitle, rcw, view_h * 0.05, bold=False)
        sub_y = bar_y + int(view_h * 0.05)
        _ctext(d, subtitle, rcx, sub_y, sf, MUTED)
        sb = d.textbbox((0, 0), subtitle, font=sf)
        sub_bot = sub_y + (sb[3] - sb[1])

        # Reserve the IP line (setup-complete) with its bottom at the cat bottom.
        # ip_h matches the subtitle size so "IP: …" reads at the same scale.
        if kind == "setup_complete":
            ip_h = int(view_h * 0.05)
            ip_anchor = (rcx, cat_bot, ip_h, rcw)
            band_bot = cat_bot - ip_h - int(view_h * 0.03)
        else:
            band_bot = cat_bot

        # QR fills the band between the subtitle and the bottom element, bottom-anchored.
        band_top = sub_bot + int(view_h * 0.05)
        qs = min(band_bot - band_top, rcw)
        qx = rcx - qs // 2
        qy = band_bot - qs
    else:
        # Portrait: cat on top, title/subtitle/QR (+IP) below.
        cs = int(view_w * 0.62)
        cx = (view_w - cs) // 2
        cy = int(view_h * 0.07)
        rad = int(cs * 0.12)
        cat, mask = _rounded_cat(cs, rad)
        img.paste(cat, (cx, cy), mask)
        d.rounded_rectangle(
            [cx - 3, cy - 3, cx + cs + 3, cy + cs + 3],
            radius=rad + 3,
            outline=GREEN,
            width=3,
        )

        cxc = view_w // 2
        ty = cy + cs + int(view_h * 0.04)
        _ctext(
            d,
            title,
            cxc,
            ty,
            _fit_font(d, title, int(view_w * 0.9), view_w * 0.12),
            WHITE,
        )
        d.rounded_rectangle(
            [
                cxc - int(view_w * 0.08),
                ty + int(view_w * 0.145),
                cxc + int(view_w * 0.08),
                ty + int(view_w * 0.165),
            ],
            radius=3,
            fill=GREEN,
        )
        _ctext(
            d,
            subtitle,
            cxc,
            ty + int(view_w * 0.20),
            _fit_font(d, subtitle, int(view_w * 0.9), view_w * 0.055, bold=False),
            MUTED,
        )

        qs = int(view_w * 0.42)
        qx = cxc - qs // 2
        qy = ty + int(view_w * 0.30)
        if kind == "setup_complete":
            ip_h = int(view_w * 0.05)
            ip_anchor = (
                cxc,
                qy + qs + int(view_w * 0.03) + ip_h,
                ip_h,
                int(view_w * 0.9),
            )

    # White QR slot (firmware draws the live QR modules into this box at runtime).
    d.rounded_rectangle(
        [qx - 6, qy - 6, qx + qs + 6, qy + qs + 6], radius=6, fill=(255, 255, 255)
    )
    return img, (qx, qy, qs), ip_anchor


def to_native(img, rect, view_w, view_h, deg):
    """Rotate the viewing-orientation image into the native panel buffer, exactly
    as the server does: native = RotateDeg(viewing, 360-deg) (CCW). `deg` is the
    device's display_rotation_deg (90 for a landscape-mounted FireBeetle whose
    panel is natively portrait). Also maps the QR rect into native coords."""
    qx, qy, qs = rect
    if deg == 0:
        return img, rect
    if deg == 90:
        # 360-90 = 270° CCW == 90° CW (PIL ROTATE_270). src(x,y) -> dst(view_h-1-y, x)
        return img.transpose(Image.ROTATE_270), (view_h - 1 - qy - qs, qx, qs)
    if deg == 270:
        # 360-270 = 90° CCW (PIL ROTATE_90). src(x,y) -> dst(y, view_w-1-x)
        return img.transpose(Image.ROTATE_90), (qy, view_w - 1 - qx - qs, qs)
    raise ValueError(f"Unsupported display_rotation_deg {deg}")


def _get_process_cli_base(width, height):
    node = shutil.which("node")
    if not node:
        raise RuntimeError("node not found")
    if not os.path.exists(PROCESS_CLI):
        raise RuntimeError(
            f"process-cli not found at {PROCESS_CLI}\n  Run: cd process-cli && npm ci"
        )
    orientation = "portrait" if height > width else "landscape"
    return [
        node,
        PROCESS_CLI,
        "--placeholder--",
        "-d",
        f"{width}x{height}",
        "--orientation",
        orientation,
        "--scale-mode",
        "cover",
    ]


def png_to_epdgz(png_path, output_dir, width, height):
    try:
        cmd = _get_process_cli_base(width, height)
        cmd[2] = png_path
        cmd.extend(["-o", output_dir, "--format", "epdgz"])
        subprocess.run(cmd, check=True)
        return True
    except RuntimeError as e:
        print(f"ERROR: {e}")
        return False
    except subprocess.CalledProcessError as e:
        print(f"ERROR: process-cli failed: {e}")
        return False


def generate_meta_header(splash_meta, setup_meta, setup_ip, rot, output_path):
    sx, sy, ss = splash_meta
    cx, cy, cs = setup_meta
    # IP line anchor for the setup-complete screen, in *viewing* (logical) coords.
    # The firmware draws it with GUI_Paint using SPLASH_ROTATE_DEG (the SAME
    # rotation baked into the pre-rotated background here) so the text lands
    # aligned + upright — NOT the runtime display_rotation_deg, which isn't set to
    # this value yet at OOBE. (The QR is drawn separately in native coords.)
    ipx, ipbottom, iph, ipmaxw = setup_ip if setup_ip else (0, 0, 0, 0)
    content = (
        "// Auto-generated splash screen metadata\n"
        "// Do not edit manually\n"
        "\n"
        "// OOBE splash screen - WiFi QR code position (native buffer coords)\n"
        f"#define SPLASH_WIFI_QR_X {sx}\n"
        f"#define SPLASH_WIFI_QR_Y {sy}\n"
        f"#define SPLASH_WIFI_QR_SIZE {ss}\n"
        "\n"
        "// Setup complete screen - web UI QR code position (native buffer coords)\n"
        f"#define SETUP_COMPLETE_QR_X {cx}\n"
        f"#define SETUP_COMPLETE_QR_Y {cy}\n"
        f"#define SETUP_COMPLETE_QR_SIZE {cs}\n"
        "\n"
        "// Setup complete screen - live IP text anchor (viewing/logical coords):\n"
        "// horizontal centre, bottom edge, and target text height. The firmware\n"
        "// draws the device IP centred at IP_CX with its bottom at IP_BOTTOM.\n"
        f"#define SETUP_COMPLETE_IP_CX {ipx}\n"
        f"#define SETUP_COMPLETE_IP_BOTTOM {ipbottom}\n"
        f"#define SETUP_COMPLETE_IP_HEIGHT {iph}\n"
        f"#define SETUP_COMPLETE_IP_MAXW {ipmaxw}\n"
        "\n"
        "// GUI_Paint rotation that matches the pre-rotated splash background, so\n"
        "// runtime-drawn text (the IP) lands aligned + upright regardless of the\n"
        "// device's stored display_rotation_deg (which is unset at OOBE).\n"
        f"#define SPLASH_ROTATE_DEG {rot}\n"
    )
    with open(output_path, "w") as f:
        f.write(content)


def main():
    sys.path.insert(0, os.path.dirname(__file__))
    from boards import BOARD_DIMENSIONS

    parser = argparse.ArgumentParser(description="Generate OOBE splash screen EPDGZ")
    g = parser.add_mutually_exclusive_group(required=True)
    g.add_argument(
        "--board",
        choices=BOARD_DIMENSIONS.keys(),
        help="Board name (display resolution from boards.py)",
    )
    g.add_argument(
        "--size",
        choices=SCREEN_SIZES.keys(),
        help="Display resolution directly (e.g. 800x480)",
    )
    parser.add_argument(
        "--output-dir",
        default=os.path.join(os.path.dirname(__file__), "..", "main", "splash_data"),
    )
    args = parser.parse_args()

    if args.board:
        native_w, native_h = NATIVE_DIMS.get(args.board, BOARD_DIMENSIONS[args.board])
        mount_landscape = MOUNT_LANDSCAPE.get(args.board, native_w >= native_h)
    else:
        native_w, native_h = SCREEN_SIZES[args.size]
        mount_landscape = native_w >= native_h

    # Compose in the viewing orientation; rotate into native when they differ.
    if (native_w >= native_h) == mount_landscape:
        view_w, view_h, rot = native_w, native_h, 0
    else:
        view_w, view_h = native_h, native_w
        rot = ROTATION_DEG.get(args.board, 90)
    os.makedirs(args.output_dir, exist_ok=True)

    all_meta = {}
    ip_meta = {}
    for name in ("splash", "setup_complete"):
        print(
            f"\n=== {name} (view {view_w}x{view_h} -> native {native_w}x{native_h}, rot {rot}) ==="
        )
        view_img, rect, ip_anchor = compose(view_w, view_h, name)
        native_img, nrect = to_native(view_img, rect, view_w, view_h, rot)
        all_meta[name] = nrect
        ip_meta[name] = ip_anchor
        print(f"  QR slot (native): x={nrect[0]} y={nrect[1]} size={nrect[2]}")
        if ip_anchor:
            print(
                f"  IP anchor (viewing): cx={ip_anchor[0]} bottom={ip_anchor[1]} h={ip_anchor[2]}"
            )

        png_path = os.path.join(args.output_dir, f"{name}.png")
        native_img.save(png_path)
        print(f"  PNG: {png_path}")

        print("  Converting to EPDGZ via process-cli...")
        if not png_to_epdgz(png_path, args.output_dir, native_w, native_h):
            print("  ERROR: Failed to generate EPDGZ")
            sys.exit(1)
        print(f"  EPDGZ: {os.path.join(args.output_dir, f'{name}.epdgz')}")

        thumb = os.path.join(args.output_dir, f"{name}.jpg")
        if os.path.exists(thumb):
            os.unlink(thumb)

    meta_path = os.path.join(args.output_dir, "splash_meta.h")
    generate_meta_header(
        all_meta["splash"],
        all_meta["setup_complete"],
        ip_meta["setup_complete"],
        rot,
        meta_path,
    )
    print(f"\n  Metadata: {meta_path}")


if __name__ == "__main__":
    main()
