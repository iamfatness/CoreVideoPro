"""Render the #456 Media page clip-range design mockup with Pillow."""

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).parent
OUT = HERE / "media-clip-range.png"
S = 2
W, H = 1500, 930
im = Image.new("RGB", (W * S, H * S), "#0c1015")
d = ImageDraw.Draw(im)


def box(x0, y0, x1, y1, fill, outline=None, radius=10, width=1):
    d.rounded_rectangle((x0*S, y0*S, x1*S, y1*S), radius=radius*S,
                        fill=fill, outline=outline, width=width*S)


def font(size, bold=False):
    name = "seguisb.ttf" if bold else "segoeui.ttf"
    return ImageFont.truetype(str(Path("C:/Windows/Fonts") / name), size*S)


def label(x, y, value, size=15, color="#dce4ec", bold=False):
    d.text((x*S, y*S), value, font=font(size, bold), fill=color)


def button(x0, y0, x1, y1, value, kind="quiet", size=13):
    colors = {
        "quiet": ("#232c36", "#495766", "#e9f0f5"),
        "green": ("#174438", "#4ad699", "#d8fff0"),
        "amber": ("#5b3d1c", "#e0a85a", "#ffe7c5"),
    }
    fill, line, fg = colors[kind]
    box(x0, y0, x1, y1, fill, line, 7)
    bounds = d.textbbox((0, 0), value, font=font(size, True))
    tw = (bounds[2] - bounds[0]) / S
    th = (bounds[3] - bounds[1]) / S
    label(x0 + ((x1-x0)-tw)/2, y0 + ((y1-y0)-th)/2 - 3,
          value, size, fg, True)


# App frame and heading.
box(20, 20, 1480, 910, "#111820", "#374350", 13)
label(44, 41, "MEDIA", 25, "#f5f8fa", True)
label(44, 77, "Cue clips to Preview, then Take when ready", 14, "#8fa0ad")
box(1110, 42, 1450, 80, "#1b252d", "#3b4d56", 7)
label(1127, 50, "PROGRAM  •  Opening Interview", 14, "#e7b36f", True)

# Left column: existing media playout.
box(40, 110, 750, 887, "#172029", "#3b4955", 10)
label(62, 130, "Media playout", 20, "#f1f5f8", True)
button(456, 126, 582, 166, "Cue to Preview", "green")
button(592, 126, 727, 166, "Play / Pause")
label(62, 175, "Ready  •  Selected clip is also on Program", 13, "#aab8c4")

# Video preview, intentionally illustrative rather than a new preview path.
box(62, 205, 728, 471, "#090e12", "#4b5b65", 6)
for y, col in [(235, "#162a38"), (280, "#213a48"), (330, "#294652"),
               (380, "#1b303d"), (430, "#101d28")]:
    d.rectangle((63*S, y*S, 727*S, (y+45)*S), fill=col)
box(255, 239, 530, 453, "#273e4c", "#577282", 4)
box(289, 250, 496, 434, "#314e5d", "#70909a", 3)
d.ellipse((358*S, 270*S, 429*S, 341*S), fill="#b69278")
box(326, 342, 462, 429, "#263744", radius=40)
label(78, 442, "EXISTING MEDIA PREVIEW", 11, "#d2dee6", True)

box(62, 486, 728, 541, "#202b35", "#425462", 6)
label(76, 494, "SELECTED ASSET", 11, "#91a2ae", True)
label(76, 511, "Opening Interview.mp4", 15, "#ecf2f5", True)
label(602, 511, "02:14.080", 14, "#c6d1d8")

# Clip range panel: the proposed UI.
box(62, 555, 728, 864, "#1c2831", "#4f6974", 8)
label(78, 570, "Clip range", 18, "#eaf3f4", True)
box(584, 571, 709, 598, "#60401f", "#d29b55", 5)
label(598, 576, "ON PROGRAM", 11, "#ffe7c8", True)
label(78, 613, "IN", 12, "#9bafb8", True)
box(113, 603, 280, 644, "#111a22", "#69808a", 5)
label(127, 611, "00:00:04.200", 17, "#f2f6f7")
button(294, 603, 559, 644, "Set from playhead")
button(570, 603, 710, 644, "Reset")
label(78, 665, "OUT", 12, "#9bafb8", True)
box(113, 655, 280, 696, "#111a22", "#69808a", 5)
label(127, 663, "00:01:38.000", 17, "#f2f6f7")
button(294, 655, 559, 696, "Set from playhead")
button(570, 655, 710, 696, "Reset")
box(78, 711, 710, 759, "#22343d", "#3f6169", 5)
label(94, 721, "Effective duration", 14, "#adc4c9")
label(545, 720, "01:33.800", 17, "#e8f4f2", True)
box(78, 774, 710, 844, "#3e3125", "#a1774e", 6)
label(94, 782, "PENDING FOR NEXT CUE", 11, "#f4bd76", True)
label(94, 803, "Current Program playback keeps its original range.", 13,
      "#f5dec3")
label(94, 822, "Cue again to apply these In / Out points.", 13,
      "#f5dec3")

# Right column: existing media bin remains visible.
box(765, 110, 1460, 887, "#172029", "#3b4955", 10)
label(787, 130, "Media bin", 20, "#f1f5f8", True)
button(1216, 126, 1314, 166, "Import")
button(1325, 126, 1438, 166, "Refresh")
label(787, 175, "3 assets available", 13, "#aab8c4")
label(787, 213, "VIDEO", 12, "#60caa8", True)


def row(y, name, duration, detail, selected=False):
    box(787, y, 1438, y+84,
        "#233b3b" if selected else "#1d2831",
        "#53b996" if selected else "#3a4a56", 7)
    label(802, y+11, name, 16, "#f1f6f8", True)
    label(802, y+38, f"{duration}  •  {detail}", 12, "#a1b3bf")
    button(1203, y+23, 1306, y+61,
           "Selected" if selected else "Select", "green" if selected else "quiet", 12)
    button(1315, y+23, 1423, y+61, "Play", "quiet", 12)


row(236, "Opening Interview.mp4", "02:14", "1920×1080  /  H.264", True)
row(332, "Guest Intro.mov", "00:26", "1920×1080  /  ProRes")
label(787, 446, "AUDIO", 12, "#60caa8", True)
row(470, "Show Theme.wav", "01:07", "48 kHz  /  stereo")
box(787, 593, 1438, 738, "#19252e", "#344c56", 8)
label(804, 612, "Operator behavior", 16, "#e5eef2", True)
label(804, 645, "•  The trim is saved with this asset, not with a scene.", 14,
      "#b6c6d0")
label(804, 672, "•  Preview and Program use the same trimmed clip.", 14,
      "#b6c6d0")
label(804, 699, "•  A clip already on air is never cut by an edit.", 14,
      "#b6c6d0")
label(787, 831, "DESIGN MOCKUP  •  #456  •  NOT A SHIPPING SCREEN", 12,
      "#7e929f", True)

im.save(OUT)
print(OUT)
