"""make_showcase.py - cuts the clips recorded in game into the TOD Gameplay+ showcase video.

  python -I make_showcase.py [--out <repo>/showcase/TOD_GameplayPlus_showcase.mp4]

Read only on the runs in <repo>/work/showcase/runs (menu m2_menu, fight f1_probe_brutal, explosion x1_vanilla /
x2_physics). Title / end cards and captions are drawn with Pillow (Segoe UI), segments cut and captioned with ffmpeg
(libx264, yuv420p, 1920x1080, 30 fps), joined, faststart. Work files go to <repo>/work/showcase/edit. Also writes
the contact sheet showcase_sheet.jpg (12 frames) next to the video.
Every caption number comes from a log; see showcase/README.md for which.
"""
import argparse, json, os, shutil, subprocess
from PIL import Image, ImageDraw, ImageFont

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = None
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
RUNS = os.path.join(ROOT, "work", "showcase", "runs")
EDIT = os.path.join(ROOT, "work", "showcase", "edit")
FONT_B = r"C:\Windows\Fonts\segoeuib.ttf"
FONT_R = r"C:\Windows\Fonts\segoeui.ttf"
FONT_BL = r"C:\Windows\Fonts\seguibl.ttf"
YELLOW, ORANGE, RED, WHITE, GREY = (255, 210, 31), (247, 147, 30), (214, 52, 24), (255, 255, 255), (200, 200, 200)
W, H, FPS = 1920, 1080, 30

MENU = os.path.join(RUNS, "m2_menu", "clips", "menu.mkv")
FIGHT = os.path.join(RUNS, "f1_probe_brutal", "clips", "fight.mkv")
VAN = os.path.join(RUNS, "x1_vanilla", "clips", "boom.mkv")
PHY = os.path.join(RUNS, "x2_physics", "clips", "boom.mkv")
BOOM_VAN, BOOM_PHY = 8.35, 8.40      # clip time of "debug boom: command_explode sent" in each run's log


def font(p, s):
    return ImageFont.truetype(p, s)


def band(d, y0, y1):
    """the game's menu look: orange -> yellow band with thin black rules"""
    for y in range(y0, y1):
        t = (y - y0) / max(1, y1 - y0 - 1)
        c = tuple(int(ORANGE[i] + (YELLOW[i] - ORANGE[i]) * t) for i in range(3))
        d.line([(0, y), (W, y)], fill=c)
    d.rectangle([0, y0 + 10, W, y0 + 14], fill=(0, 0, 0)); d.rectangle([0, y1 - 14, W, y1 - 10], fill=(0, 0, 0))


def centered(d, y, text, f, fill):
    w = d.textlength(text, font=f)
    d.text(((W - w) / 2, y), text, font=f, fill=fill)


def title_card(fn):
    im = Image.new("RGB", (W, H), (8, 6, 4)); d = ImageDraw.Draw(im)
    band(d, 300, 640)
    centered(d, 352, "TOTAL OVERDOSE", font(FONT_BL, 64), (20, 12, 6))
    centered(d, 430, "Gameplay+", font(FONT_BL, 150), (20, 12, 6))
    centered(d, 700, "New difficulty levels  \u00b7  Controller support  \u00b7  Box3D physics", font(FONT_B, 44), YELLOW)
    centered(d, 770, "A mod for the original 2005 PC game", font(FONT_R, 32), GREY)
    im.save(fn)


def end_card(fn):
    im = Image.new("RGB", (W, H), (8, 6, 4)); d = ImageDraw.Draw(im)
    band(d, 120, 300)
    centered(d, 150, "Total Overdose \u2014 Gameplay+", font(FONT_BL, 84), (20, 12, 6))
    items = [("Difficulty", "EASY to PSYCHO, in the game's own SETTINGS menu"),
             ("Brutal", "tougher enemies, reinforcements, up to 9 enemies at once"),
             ("Controller", "Xbox and PlayStation pads, rumble, matching button prompts"),
             ("Box3D physics", "loose props react to bullets and explosions (prototype)")]
    y = 390
    fk, fv = font(FONT_B, 44), font(FONT_R, 36)
    for k, v in items:
        d.text((260, y), k, font=fk, fill=YELLOW)
        d.text((680, y + 6), v, font=fv, fill=WHITE)
        assert 680 + d.textlength(v, font=fv) < W - 120, v
        y += 110
    centered(d, 880, "For the GOG release of Total Overdose (2005)", font(FONT_R, 30), GREY)
    im.save(fn)


def caption(fn, title, sub=None, where="bottom", chips=None):
    """lower third (or top bar on menus): dark rounded panel, orange accent, yellow title, white line under it"""
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    ft, fs = font(FONT_B, 44), font(FONT_R, 32)
    tw = d.textlength(title, font=ft)
    sw = d.textlength(sub, font=fs) if sub else 0
    cw = 0
    if chips:
        fc = font(FONT_B, 34)
        cw = sum(d.textlength(c, font=fc) + 56 for c, _ in chips) + 16 * (len(chips) - 1)
    pw = int(max(tw, sw, cw) + 80)
    ph = 74 + (48 if sub else 0) + (64 if chips else 0)
    x0 = (W - pw) // 2
    y0 = 985 - ph if where == "bottom" else 14   # bottom: above the health bar, between radar and ammo
    d.rounded_rectangle([x0, y0, x0 + pw, y0 + ph], radius=14, fill=(0, 0, 0, 200))
    d.rounded_rectangle([x0, y0, x0 + 10, y0 + ph], radius=5, fill=ORANGE + (255,))
    d.text((x0 + 40, y0 + 10), title, font=ft, fill=YELLOW)
    y = y0 + 68
    if sub:
        d.text((x0 + 40, y), sub, font=fs, fill=WHITE); y += 48
    if chips:
        x = x0 + 40
        for c, on in chips:
            w = d.textlength(c, font=fc) + 56
            if on:
                d.rounded_rectangle([x, y, x + w, y + 52], radius=26, fill=YELLOW + (255,))
                d.text((x + 28, y + 5), c, font=fc, fill=(20, 12, 6))
            else:
                d.rounded_rectangle([x, y, x + w, y + 52], radius=26, outline=GREY + (255,), width=2)
                d.text((x + 28, y + 5), c, font=fc, fill=GREY)
            x += w + 16
    im.save(fn)


def run(cmd):
    subprocess.run([FFMPEG, "-y", "-loglevel", "error"] + cmd, check=True)


ENC = ["-c:v", "libx264", "-preset", "slow", "-crf", "18", "-pix_fmt", "yuv420p", "-r", str(FPS), "-an"]


def still(png, secs, out):
    run(["-loop", "1", "-framerate", str(FPS), "-t", str(secs), "-i", png,
         "-vf", "fade=in:st=0:d=0.5,fade=out:st=%.2f:d=0.5,format=yuv420p" % (secs - 0.5)] + ENC + [out])


def segment(src, ss, dur, caps, out, fade=0.25, fin=None, fout=None):
    """src cut [ss, ss+dur]; caps = [(png, t0, t1)] in segment time, each faded in / out over 0.25 s"""
    args = ["-ss", "%.3f" % ss, "-t", "%.3f" % dur, "-i", src]
    for png, t0, t1 in caps:
        args += ["-loop", "1", "-framerate", str(FPS), "-t", "%.3f" % dur, "-i", png]
    chain = "[0:v]fps=%d,scale=%d:%d,setsar=1,format=yuv420p[v0]" % (FPS, W, H)
    last = "v0"
    for i, (png, t0, t1) in enumerate(caps, 1):
        chain += ";[%d:v]format=rgba,fade=in:st=%.2f:d=0.25:alpha=1,fade=out:st=%.2f:d=0.25:alpha=1[c%d]" % (i, t0, t1 - 0.25, i)
        chain += ";[%s][c%d]overlay=0:0:enable='between(t,%.2f,%.2f)'[v%d]" % (last, i, t0, t1, i)
        last = "v%d" % i
    fin = fade if fin is None else fin; fout = fade if fout is None else fout
    fx = ["fade=in:st=0:d=%.2f" % fin] if fin else []
    fx += ["fade=out:st=%.2f:d=%.2f" % (dur - fout, fout)] if fout else []
    chain += ";[%s]%s[out]" % (last, ",".join(fx) or "null")
    run(args + ["-filter_complex", chain, "-map", "[out]"] + ENC + [out])


def side_by_side(a, a_ss, b, b_ss, dur, labels, cap, out):
    lab = os.path.join(EDIT, "sbs_labels.png")
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    f = font(FONT_B, 40)
    for i, t in enumerate(labels):
        x = i * 960 + (960 - d.textlength(t, font=f)) / 2
        d.text((x, 196), t, font=f, fill=YELLOW if i else WHITE)
    im.save(lab)
    chain = ("[0:v]fps=%d,scale=960:540,setsar=1[a];[1:v]fps=%d,scale=960:540,setsar=1[b];"
             "color=c=0x080604:s=%dx%d:r=%d:d=%.3f[bg];[bg][a]overlay=0:270[x];[x][b]overlay=960:270[y];"
             "[y][2:v]overlay=0:0[z];[z][3:v]overlay=0:0,format=yuv420p,fade=in:st=0:d=0.25,fade=out:st=%.2f:d=0.25[out]"
             % (FPS, FPS, W, H, FPS, dur, dur - 0.25))
    run(["-ss", "%.3f" % a_ss, "-t", "%.3f" % dur, "-i", a, "-ss", "%.3f" % b_ss, "-t", "%.3f" % dur, "-i", b,
         "-loop", "1", "-framerate", str(FPS), "-t", "%.3f" % dur, "-i", lab,
         "-loop", "1", "-framerate", str(FPS), "-t", "%.3f" % dur, "-i", cap,
         "-filter_complex", chain, "-map", "[out]"] + ENC + [out])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "showcase", "TOD_GameplayPlus_showcase.mp4"))
    a = ap.parse_args()
    if os.path.exists(EDIT):
        shutil.rmtree(EDIT)
    os.makedirs(EDIT)
    P = lambda n: os.path.join(EDIT, n)   # noqa: E731
    title_card(P("title.png")); end_card(P("end.png"))
    caption(P("c_diff1.png"), "New difficulty levels", "SETTINGS \u203a DIFFICULTY LEVEL: Easy \u00b7 Normal \u00b7 Hard \u00b7 Brutal \u00b7 Psycho", "top")
    caption(P("c_diff2.png"), "Brutal and Psycho", "Harder enemies, reinforcements, more enemies at once", "top")
    caption(P("c_vib.png"), "VIBRATION switch for controllers", "In the same SETTINGS page", "top")
    pads = ["Keyboard", "Xbox", "PlayStation"]
    for i, n in enumerate(pads):
        caption(P("c_pad%d.png" % i), "Button prompts follow the device you use", None, "top", [(p, j == i) for j, p in enumerate(pads)])
    caption(P("c_brutal1.png"), "BRUTAL: more enemies at once",
            "Live-enemy cap raised from the game's 6 to 9 \u2014 this fight reached 9 alive at once")
    caption(P("c_brutal2.png"), "Fallen enemies come back as reinforcements",
            "Test run of The Ambush: 7 enemies + 3 reinforcements = 10")
    caption(P("c_van.png"), "Vanilla", "The game's own explosion: the crates do not move")
    caption(P("c_phy.png"), "Box3D physics", "Same explosion: crates thrown 14\u201325 m, the chair 34 m, tumbling and settling")
    caption(P("c_sbs.png"), "Same keys, same explosion", "Left vanilla, right Box3D physics")

    segs = []
    def add(name):
        segs.append(P(name)); return P(name)
    still(P("title.png"), 3.5, add("s1_title.mp4"))
    # 2. difficulty: SETTINGS opens at clip 5.6 s (debug focus log 16:40:02.07, clip start 16:39:56.49); presses at
    #    8.0 NORMAL, 9.8 HARD, 11.7 BRUTAL, 13.5 PSYCHO; DOWN DOWN 15.6 / 16.3 (VIBRATION row)
    segment(MENU, 5.3, 12.6, [(P("c_diff1.png"), 0.3, 6.2), (P("c_diff2.png"), 6.4, 10.3), (P("c_vib.png"), 10.5, 12.5)], add("s2_difficulty.mp4"))
    # 3. prompts: keyboard SETTINGS 30.0-32.9, Xbox 21.95-24.25, PlayStation 25.75-28.35 (log: page opens / style lines)
    segment(MENU, 30.0, 2.9, [(P("c_pad0.png"), 0.0, 2.9)], add("s3a_keyboard.mp4"), fade=0.15)
    segment(MENU, 21.95, 2.3, [(P("c_pad1.png"), 0.0, 2.3)], add("s3b_xbox.mp4"), fade=0.15)
    segment(MENU, 25.75, 2.6, [(P("c_pad2.png"), 0.0, 2.6)], add("s3c_playstation.mp4"), fade=0.15)
    # 4. BRUTAL fight (f1, no kill timer): clip 15.0-20.9 and 21.6-29.6 (20.9-21.6: the camera inside a wall, cut out)
    segment(FIGHT, 15.0, 5.9, [(P("c_brutal1.png"), 0.3, 5.9)], add("s4a_brutal.mp4"), fout=0)
    segment(FIGHT, 21.6, 8.0, [(P("c_brutal2.png"), 0.0, 7.7)], add("s4b_brutal.mp4"), fin=0)
    # 5. physics: vanilla then Box3D, both from 3 s before the explosion, then side by side
    segment(VAN, BOOM_VAN - 3.0, 7.5, [(P("c_van.png"), 0.3, 7.3)], add("s5a_vanilla.mp4"))
    segment(PHY, BOOM_PHY - 3.0, 9.5, [(P("c_phy.png"), 0.3, 9.3)], add("s5b_physics.mp4"))
    side_by_side(VAN, BOOM_VAN - 1.0, PHY, BOOM_PHY - 1.0, 6.5, ["Vanilla", "Box3D physics"], P("c_sbs.png"), add("s5c_side_by_side.mp4"))
    still(P("end.png"), 4.0, add("s6_end.mp4"))

    lst = P("concat.txt")
    open(lst, "w", encoding="utf-8").write("".join("file '%s'\n" % s.replace("\\", "/") for s in segs))
    run(["-f", "concat", "-safe", "0", "-i", lst, "-c", "copy", "-movflags", "+faststart", a.out])
    # timestamps for the README / report
    t, marks = 0.0, []
    for s in segs:
        out = subprocess.run([FFMPEG, "-i", s], capture_output=True, text=True).stderr
        dur = out.split("Duration: ")[1].split(",")[0]
        h, m, sec = dur.split(":"); d = int(h) * 3600 + int(m) * 60 + float(sec)
        marks.append((os.path.basename(s), round(t, 2), round(d, 2))); t += d
    json.dump(marks, open(P("segments.json"), "w"), indent=1)
    for m in marks:
        print("%-24s at %6.2f s, %5.2f s" % m)
    print("total %.2f s, %.1f MB" % (t, os.path.getsize(a.out) / 1e6))
    # contact sheet: 12 frames spread over the video
    sheet = os.path.join(os.path.dirname(a.out), "showcase_sheet.jpg")
    times = [t * (i + 0.5) / 12 for i in range(12)]
    ims = []
    for i, tt in enumerate(times):
        fn = P("sheet_%02d.png" % i)
        run(["-ss", "%.2f" % tt, "-i", a.out, "-frames:v", "1", "-vf", "scale=640:360", fn])
        ims.append(Image.open(fn).convert("RGB"))
    sh = Image.new("RGB", (640 * 4, 360 * 3))
    for i, im in enumerate(ims):
        sh.paste(im, ((i % 4) * 640, (i // 4) * 360))
    sh.save(sheet, quality=88)
    print("sheet", sheet)


if __name__ == "__main__":
    main()
