#!/usr/bin/env python3
"""Generates the wave 1 style packs (graphics styles spec 6.3, slice 10).

Writes native/resources/graphics-packs/<id>/pack.json, palettes/*.json and
templates/*.json (the slice 7 pack format, docs/reference/graphics-packs.md). The JSON
files are the product (the core loads them); this script is how they were
authored, kept so a change to a shared convention (field names, motion,
placement) lands in every template at once. Run from the repo root:

    python scripts/graphics/generate-style-packs.py

Conventions (spec 6.2): colours are brand tokens; a raw hex colour appears only
on a node that says "fixed": true (a white LIVE dot, a highlight line). Fonts
are the pack's own OFL faces (fonts/; provenance in graphics-packs/SOURCE.md).
Every template stays inside the template limits (64 layers, 32 fields,
3000 ms of motion) and uses only primitives on main: text tracking, italic and
caps (P1), angled/multi-stop/radial gradients (P2), polygon and skew plates
(P3), shape shadow and glow (P4), and slice 8 motion: feathered reveals,
line/word builds and looping accents (P5-P7), with the expo-out / quint-out
pack easings, and true backdrop blur (P9: "glass" plates, compositor-blurred
Program behind a rounded rect; never imitated with a frosted fill).
"""
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'native', 'resources', 'graphics-packs')


def tok(name):
    return {'token': 'brand.' + name}


def fld(name):
    return {'field': name}


def node(kind, ident, x, y, w, h, **props):
    n = {'id': ident, 'type': kind, 'x': x, 'y': y, 'width': w, 'height': h}
    n.update(props)
    return n


def rect(ident, x, y, w, h, fill, **props):
    return node('rect', ident, x, y, w, h, fill=fill, **props)


def ellipse(ident, x, y, w, h, fill, **props):
    return node('ellipse', ident, x, y, w, h, fill=fill, **props)


def text(ident, x, y, w, h, value, font, size, weight, color, **props):
    n = node('text', ident, x, y, w, h, text=value, font=font, fontSize=size, fontWeight=weight,
             lineHeight=props.pop('lineHeight', 1.2), color=color, align=props.pop('align', 'left'),
             maxLines=props.pop('maxLines', 1), overflow=props.pop('overflow', 'ellipsis'))
    n.update(props)
    return n


def fixed(n):
    """Marks a node whose colours are deliberately raw hex (spec 6.2 lint)."""
    n['fixed'] = True
    return n


def motion(kind, start, duration, easing='ease-out', **extra):
    m = {'type': kind, 'startMs': start, 'durationMs': duration, 'easing': easing}
    m.update(extra)
    return m


def animate(n, i=None, o=None):
    n['animations'] = {}
    if i:
        n['animations']['in'] = i
    if o:
        n['animations']['out'] = o
    return n


def field(ident, default, required=False, max_length=256, kind='string'):
    f = {'id': ident, 'type': kind, 'default': default, 'bindable': True, 'maxLength': max_length}
    if required:
        f['required'] = True
    return f


def tokens_in(value, found):
    if isinstance(value, dict):
        if set(value) == {'token'}:
            found.add(value['token'])
        for v in value.values():
            tokens_in(v, found)
    elif isinstance(value, list):
        for v in value:
            tokens_in(v, found)


def count_layers(layers):
    return sum(1 + count_layers(n.get('children', [])) for n in layers)


def shadow_margin(layers):
    reach = 0
    for n in layers:
        s = n.get('shadow')
        if s and n['type'] != 'text':
            reach = max(reach, s.get('spread', 0) + 1.5 * s.get('blur', 0) + max(abs(s.get('dx', 0)), abs(s.get('dy', 0))) + 2)
        reach = max(reach, shadow_margin(n.get('children', [])))
    return int(-(-reach // 4) * 4)


def template(pack, slug, name, family, width, height, fields, layers, placement, graphic_in=None,
             graphic_out=None):
    # Shadows and glows paint outside their shape, and the template bounds clip
    # everything: grow the bounds by the shadow's reach on every side, move the
    # layers in by the same margin, and move the placement out so the plate
    # lands where it would without one.
    margin = shadow_margin(layers)
    if margin:
        for n in layers:
            n['x'] += margin
            n['y'] += margin
        width, height = width + 2 * margin, height + 2 * margin
        placement = dict(placement)
        vertical, horizontal = placement['anchor'].split('-') if '-' in placement['anchor'] else ('center', 'center')
        if horizontal == 'left':
            placement['x'] -= margin
        elif horizontal == 'right':
            placement['x'] += margin
        if vertical == 'upper':
            placement['y'] -= margin
        elif vertical == 'lower':
            placement['y'] += margin
    found = set()
    tokens_in(layers, found)
    doc = {
        'schemaVersion': 1, 'templateId': pack + '-' + slug, 'templateVersion': 1, 'name': name, 'family': family,
        'canvas': {'width': 1920, 'height': 1080}, 'bounds': {'width': width, 'height': height},
        'brandTokens': sorted(found), 'fields': fields, 'layers': layers,
        'animations': {'in': graphic_in or {'type': 'fade', 'durationMs': 200, 'easing': 'ease-out'},
                       'out': graphic_out or {'type': 'fade', 'durationMs': 200, 'easing': 'ease-in'}},
        'defaultPlacement': placement,
        'metadata': {'pack': pack, 'generator': 'scripts/graphics/generate-style-packs.py'},
    }
    assert count_layers(layers) <= 64, (slug, count_layers(layers))
    assert len(fields) <= 32
    return slug, doc


LOWER_LEFT = {'anchor': 'lower-left', 'x': 96, 'y': -64, 'scale': 1}
LOWER_CENTER = {'anchor': 'lower-center', 'x': 0, 'y': -64, 'scale': 1}
CENTER = {'anchor': 'center', 'x': 0, 'y': 0, 'scale': 1}
UPPER_RIGHT = {'anchor': 'upper-right', 'x': -64, 'y': 48, 'scale': 1}
UPPER_LEFT = {'anchor': 'upper-left', 'x': 64, 'y': 48, 'scale': 1}
BOTTOM = {'anchor': 'lower-center', 'x': 0, 'y': 0, 'scale': 1}

PACKS = {}


def pack(ident, name, genre, fonts, palettes, templates):
    PACKS[ident] = {'name': name, 'genre': genre, 'fonts': fonts, 'palettes': palettes, 'templates': templates}


def palette(name, primary, secondary, accent, surface, on_primary, on_primary_muted, on_secondary, on_accent,
            team_a=None, team_b=None):
    colors = {'brand.primary': primary, 'brand.secondary': secondary, 'brand.accent': accent,
              'brand.surface': surface, 'brand.onPrimary': on_primary, 'brand.onPrimaryMuted': on_primary_muted,
              'brand.onSecondary': on_secondary, 'brand.onAccent': on_accent}
    if team_a:
        colors['brand.teamA'] = team_a
        colors['brand.teamB'] = team_b
    return colors


# ── News: Signal ────────────────────────────────────────────────────────────
# Stacked name bar over a secondary title bar, hard accent strip at left, red
# tag, subtle top-down gradient and a 1 px highlight line. Strip wipes up
# (200 ms), plate reveals left to right (400 ms), text follows; out 70 %.
def news():
    F = 'Barlow Semi Condensed'
    p = 'news-signal'
    name_f = field('name', 'Ada Otieno', True)
    title_f = field('title', 'Chief Political Correspondent', max_length=512)
    tag_f = field('tag', 'LIVE', max_length=32)

    def strip(h):
        return animate(rect('strip', 0, 0, 10, h, tok('accent')),
                       motion('wipe', 0, 200, 'expo-out', direction='down'), motion('wipe', 0, 140, 'ease-in', direction='down'))

    def name_bar(w, h, y=0):
        n = animate(rect('name-bar', 10, y, w - 10, h,
                            {'type': 'linear', 'angle': 180, 'stops': [tok('primary'), tok('surface')]}),
                       motion('reveal', 150, 400, 'expo-out', direction='left', feather=24), motion('reveal', 0, 280, 'ease-in', direction='left', feather=24))
        n['loop'] = {'type': 'sweep', 'periodMs': 8000, 'durationMs': 900, 'direction': 'left'}
        return n

    def highlight(w, y=0):
        return animate(fixed(rect('highlight', 10, y, w - 10, 2, '#FFFFFF33')),
                       motion('reveal', 150, 400, 'expo-out', direction='left', feather=24), motion('reveal', 0, 280, 'ease-in', direction='left', feather=24))

    def name_text(w, y, size=50):
        return animate(text('name-text', 38, y, w - 60, int(size * 1.25), fld('name'), F, size, 600, tok('onPrimary')),
                       motion('rise', 250, 300, 'quint-out', distance=12), motion('fade', 0, 140, 'ease-in'))

    lt1 = template(p, 'lower-third', 'News Signal lower third', 'lower-third', 1080, 96, [name_f], [
        strip(96), name_bar(1080, 96), highlight(1080), name_text(1080, 16)], LOWER_LEFT)
    lt2 = template(p, 'lower-third-2-line', 'News Signal two-line', 'lower-third', 1080, 150, [name_f, title_f], [
        strip(150), name_bar(1080, 92), highlight(1080),
        animate(rect('title-bar', 10, 92, 860, 58, tok('secondary')),
                motion('reveal', 250, 400, 'expo-out', direction='left', feather=24), motion('reveal', 0, 280, 'ease-in', direction='left', feather=24)),
        name_text(1080, 14),
        animate(text('title-text', 38, 100, 820, 42, fld('title'), F, 32, 400, tok('onSecondary')),
                motion('rise', 350, 300, 'quint-out', distance=12), motion('fade', 0, 140, 'ease-in'))], LOWER_LEFT)
    tag = template(p, 'name-tag', 'News Signal name with tag', 'lower-third', 1080, 150, [name_f, title_f, tag_f], [
        strip(150), name_bar(1080, 92), highlight(1080),
        animate(rect('title-bar', 10, 92, 860, 58, tok('secondary')),
                motion('reveal', 250, 400, 'expo-out', direction='left', feather=24), motion('reveal', 0, 280, 'ease-in', direction='left', feather=24)),
        animate(rect('tag-plate', 900, 92, 180, 58, tok('accent')),
                motion('reveal', 350, 300, 'expo-out', direction='left', feather=16), motion('reveal', 0, 200, 'ease-in', direction='left', feather=16)),
        name_text(1080, 14),
        animate(text('title-text', 38, 100, 840, 42, fld('title'), F, 32, 400, tok('onSecondary')),
                motion('rise', 350, 300, 'quint-out', distance=12), motion('fade', 0, 140, 'ease-in')),
        animate(text('tag-text', 900, 102, 180, 40, fld('tag'), F, 30, 700, tok('onAccent'), align='center',
                     textTransform='uppercase', letterSpacing=0.06),
                motion('rise', 350, 300, 'quint-out', distance=12), motion('fade', 0, 140, 'ease-in'))], LOWER_LEFT)
    title = template(p, 'title', 'News Signal full-width title', 'title', 1920, 180,
                     [field('heading', 'Election Night Special', True), field('subtitle', 'Results as they come in', max_length=512)], [
        animate(rect('plate', 0, 0, 1920, 124, {'type': 'linear', 'angle': 180, 'stops': [tok('primary'), tok('surface')]}),
                motion('reveal', 0, 400, 'expo-out', direction='left', feather=32), motion('reveal', 0, 280, 'ease-in', direction='left', feather=32)),
        animate(rect('rule', 0, 124, 1920, 6, tok('accent')),
                motion('reveal', 100, 400, 'expo-out', direction='left', feather=32), motion('reveal', 0, 280, 'ease-in', direction='left', feather=32)),
        animate(rect('sub-bar', 0, 130, 1920, 50, tok('secondary')),
                motion('reveal', 200, 400, 'expo-out', direction='left', feather=32), motion('reveal', 0, 280, 'ease-in', direction='left', feather=32)),
        animate(text('heading-text', 96, 22, 1728, 84, fld('heading'), F, 68, 700, tok('onPrimary'), textTransform='uppercase',
                     letterSpacing=0.02), motion('rise', 250, 300, 'quint-out', distance=12), motion('fade', 0, 140, 'ease-in')),
        animate(text('subtitle-text', 96, 134, 1728, 42, fld('subtitle'), F, 32, 400, tok('onSecondary')),
                motion('rise', 350, 300, 'quint-out', distance=12), motion('fade', 0, 140, 'ease-in'))], BOTTOM)
    bug = template(p, 'bug', 'News Signal corner bug', 'bug', 300, 72, [field('channel', 'NEWS 24', True, 32), tag_f], [
        rect('plate', 0, 0, 300, 72, tok('primary')),
        rect('tag-plate', 0, 0, 104, 72, tok('accent')),
        fixed(ellipse('dot', 12, 29, 14, 14, '#FFFFFFFF')),
        text('tag-text', 32, 16, 68, 40, fld('tag'), F, 28, 700, tok('onAccent'), textTransform='uppercase'),
        text('channel-text', 116, 14, 172, 44, fld('channel'), F, 34, 700, tok('onPrimary'), textTransform='uppercase',
             letterSpacing=0.04)], UPPER_RIGHT)
    clock = template(p, 'clock-bug', 'News Signal clock bug', 'clock', 280, 72, [field('time', '19:42', True, 32), field('zone', 'ET', max_length=16)], [
        rect('plate', 0, 0, 280, 72, tok('primary')),
        rect('strip', 0, 0, 8, 72, tok('accent')),
        text('time-text', 24, 10, 170, 52, fld('time'), F, 44, 700, tok('onPrimary')),
        text('zone-text', 190, 20, 74, 34, fld('zone'), F, 26, 600, tok('onPrimaryMuted'), align='right', textTransform='uppercase')], UPPER_LEFT)
    ticker = template(p, 'ticker', 'News Signal ticker', 'lower-third', 1920, 64,
                      [field('label', 'BREAKING', max_length=32), field('items', '["Polls close in the east at eight","Turnout up on last cycle","Live results all night"]', True, 4096)], [
        rect('band', 0, 0, 1920, 64, tok('surface')),
        rect('label-plate', 0, 0, 260, 64, tok('accent')),
        node('crawl', 'crawl', 280, 10, 1620, 44, items=fld('items'), font=F, fontSize=38, color=tok('onPrimary'), speed=140, gap=96),
        text('label-text', 0, 12, 260, 40, fld('label'), F, 32, 700, tok('onAccent'), align='center', textTransform='uppercase',
             letterSpacing=0.06)], BOTTOM)
    pack(p, 'News: Signal', 'news',
         [{'family': F, 'files': ['BarlowSemiCondensed-Regular.ttf', 'BarlowSemiCondensed-SemiBold.ttf', 'BarlowSemiCondensed-Bold.ttf']}],
         {'navy-red': palette('Navy / Red', '#0B1F3AFF', '#13315CFF', '#E63946FF', '#0A0F18B3', '#FFFFFFFF', '#C9D3E2FF', '#D8E2F0FF', '#FFFFFFFF'),
          'charcoal-gold': palette('Charcoal / Gold', '#26282CFF', '#F2EEE4FF', '#E2A72EFF', '#17181BFF', '#FFFFFFFF', '#D4D0C6FF', '#26282CFF', '#17181BFF'),
          'white-blue': palette('White / Blue', '#FFFFFFFF', '#1B4F9CFF', '#1B4F9CFF', '#E6EBF2FF', '#14181FFF', '#4A5363FF', '#FFFFFFFF', '#FFFFFFFF')},
         [lt1, lt2, tag, title, bug, clock, ticker])


# ── Sports: Kinetic ─────────────────────────────────────────────────────────
# 12-degree skewed plates in team colour, dark stat block, diagonal speed
# lines at 15 %. Bold italic caps, tight tracking. Plates slam in from the
# side, staggered 60 ms; text snaps.
def sports():
    F = 'Barlow Condensed'
    p = 'sports-kinetic'
    name_f = field('name', 'Marcus Reyes', True)
    title_f = field('title', 'Forward  |  #9', max_length=512)

    def slam(n, start):
        return animate(n, motion('slide', start, 250, 'expo-out', direction='left', distance=160),
                       motion('slide', 0, 180, 'ease-in', direction='left', distance=160))

    def snap(n, start):
        return animate(n, motion('cut', start, 0), motion('cut', 0, 0))

    def speed_lines(x, y, w, h, count=5):
        lines = []
        step = w // count
        for i in range(count):
            lines.append(fixed(node('line', 'speed-%d' % i, x + i * step, y, h // 2, h, diagonal='up',
                                    stroke='#FFFFFFFF', strokeWidth=3, opacity=0.15)))
        return {'id': 'speed-lines', 'type': 'group', 'x': 0, 'y': 0, 'width': x + w, 'height': y + h, 'children': lines}

    def caps(ident, x, y, w, h, value, size, color, weight=700, **props):
        return text(ident, x, y, w, h, value, F, size, weight, color, fontStyle='italic', textTransform='uppercase',
                    letterSpacing=props.pop('letterSpacing', -0.01), **props)

    lt1 = template(p, 'lower-third', 'Sports Kinetic lower third', 'lower-third', 1000, 100, [name_f], [
        slam(rect('name-plate', 0, 0, 1000, 100, tok('teamA'), skewX=12,
                  shadow={'color': tok('surface'), 'dx': 8, 'dy': 8, 'blur': 0}), 0),
        speed_lines(640, 0, 300, 100),
        snap(caps('name-text', 48, 10, 860, 80, fld('name'), 66, tok('onPrimary')), 250)], LOWER_LEFT)
    lt2 = template(p, 'lower-third-2-line', 'Sports Kinetic two-line', 'lower-third', 1000, 160, [name_f, title_f], [
        slam(rect('name-plate', 0, 0, 1000, 100, tok('teamA'), skewX=12,
                  shadow={'color': tok('surface'), 'dx': 8, 'dy': 8, 'blur': 0}), 0),
        slam(rect('title-plate', 60, 104, 640, 56, tok('surface'), skewX=12), 60),
        speed_lines(640, 0, 300, 100),
        snap(caps('name-text', 48, 10, 860, 80, fld('name'), 66, tok('onPrimary')), 250),
        snap(caps('title-text', 100, 110, 560, 44, fld('title'), 34, tok('onSecondary'), 600), 310)], LOWER_LEFT)
    tag = template(p, 'name-tag', 'Sports Kinetic name with number', 'lower-third', 1000, 120,
                   [name_f, title_f, field('number', '9', max_length=4)], [
        slam(rect('number-block', 0, 0, 150, 120, tok('surface'), skewX=12), 0),
        slam(rect('name-plate', 130, 0, 870, 76, tok('teamA'), skewX=12), 60),
        slam(rect('title-plate', 120, 80, 600, 40, tok('secondary'), skewX=12), 120),
        snap(caps('number-text', 0, 10, 150, 100, fld('number'), 88, tok('onPrimary'), align='center'), 250),
        snap(caps('name-text', 180, 6, 780, 64, fld('name'), 54, tok('onPrimary')), 280),
        snap(caps('title-text', 160, 82, 540, 36, fld('title'), 28, tok('onSecondary'), 600), 310)], LOWER_LEFT)
    title = template(p, 'title', 'Sports Kinetic full-width title', 'title', 1920, 200,
                     [field('heading', 'Championship Final', True), field('subtitle', 'Live from City Stadium', max_length=512)], [
        slam(rect('plate', 0, 20, 1920, 130, tok('teamA')), 0),
        slam(rect('sub-plate', 360, 150, 1200, 50, tok('surface'), skewX=-12), 60),
        speed_lines(1400, 20, 420, 130, 6),
        snap(caps('heading-text', 96, 30, 1728, 110, fld('heading'), 96, tok('onPrimary'), align='center', letterSpacing=0.0), 250),
        snap(caps('subtitle-text', 400, 154, 1120, 42, fld('subtitle'), 32, tok('onSecondary'), 600, align='center'), 310)], BOTTOM)
    bug = template(p, 'bug', 'Sports Kinetic corner bug', 'bug', 240, 80, [field('channel', 'LIVE', True, 32)], [
        rect('plate', 0, 0, 240, 80, tok('teamA'), skewX=12),
        caps('channel-text', 50, 8, 140, 64, fld('channel'), 54, tok('onPrimary'), align='center')], UPPER_RIGHT)
    score = template(p, 'score-bug', 'Sports Kinetic score bug', 'bug', 620, 72,
                     [field('team-a', 'CITY', True, 8), field('score-a', '2', max_length=4), field('team-b', 'ROVERS', True, 8),
                      field('score-b', '1', max_length=4), field('clock', '67:12', max_length=12), field('period', '2ND', max_length=8)], [
        rect('team-a-plate', 0, 0, 170, 72, tok('teamA')),
        rect('score-plate', 170, 0, 140, 72, tok('surface')),
        rect('team-b-plate', 310, 0, 170, 72, tok('teamB')),
        rect('clock-plate', 480, 0, 140, 72, tok('primary')),
        caps('team-a-text', 0, 8, 170, 56, fld('team-a'), 44, tok('onPrimary'), align='center'),
        caps('score-a-text', 170, 4, 70, 64, fld('score-a'), 54, tok('onSecondary'), align='center'),
        caps('score-b-text', 240, 4, 70, 64, fld('score-b'), 54, tok('onSecondary'), align='center'),
        caps('team-b-text', 310, 8, 170, 56, fld('team-b'), 44, tok('onPrimary'), align='center'),
        caps('clock-text', 480, 2, 140, 44, fld('clock'), 36, tok('onPrimary'), align='center'),
        caps('period-text', 480, 40, 140, 30, fld('period'), 24, tok('onPrimaryMuted'), 600, align='center')], UPPER_LEFT)
    stat = template(p, 'stat-lower-third', 'Sports Kinetic stat lower third', 'lower-third', 1100, 150,
                    [name_f, field('stat-1', '24', max_length=8), field('label-1', 'Goals', max_length=24),
                     field('stat-2', '11', max_length=8), field('label-2', 'Assists', max_length=24),
                     field('stat-3', '89%', max_length=8), field('label-3', 'Pass accuracy', max_length=24)], [
        slam(rect('name-plate', 0, 0, 1100, 70, tok('teamA'), skewX=12), 0),
        slam(rect('stat-block', 30, 74, 1040, 76, tok('surface'), skewX=12), 60),
        snap(caps('name-text', 40, 6, 1000, 58, fld('name'), 50, tok('onPrimary')), 250),
        snap(caps('stat-1-text', 80, 78, 120, 66, fld('stat-1'), 56, tok('accent')), 310),
        snap(caps('label-1-text', 200, 96, 160, 36, fld('label-1'), 28, tok('onSecondary'), 600), 310),
        snap(caps('stat-2-text', 400, 78, 120, 66, fld('stat-2'), 56, tok('accent')), 310),
        snap(caps('label-2-text', 520, 96, 160, 36, fld('label-2'), 28, tok('onSecondary'), 600), 310),
        snap(caps('stat-3-text', 720, 78, 130, 66, fld('stat-3'), 56, tok('accent')), 310),
        snap(caps('label-3-text', 850, 96, 200, 36, fld('label-3'), 28, tok('onSecondary'), 600), 310)], LOWER_LEFT)
    team = lambda name, a, b, prim, sec, acc: palette(name, prim, sec, acc, '#101216F2', '#FFFFFFFF', '#D8DCE3FF', '#FFFFFFFF', '#101216FF', a, b)
    pack(p, 'Sports: Kinetic', 'sports',
         [{'family': F, 'files': ['BarlowCondensed-Regular.ttf', 'BarlowCondensed-Bold.ttf', 'BarlowCondensed-SemiBoldItalic.ttf', 'BarlowCondensed-BoldItalic.ttf']}],
         {'home-red': team('Home Red', '#C8102EFF', '#1D428AFF', '#14171CFF', '#2A2F38FF', '#FFC72CFF'),
          'away-blue': team('Away Blue', '#1D428AFF', '#C8102EFF', '#0F1A2EFF', '#26324AFF', '#7FD1FFFF'),
          'night': team('Night', '#00A86BFF', '#F2A900FF', '#0B0D10FF', '#1C2026FF', '#00E08FFF')},
         [lt1, lt2, tag, title, bug, score, stat])


# ── Worship: Grace ──────────────────────────────────────────────────────────
# No hard box: a soft dark gradient rising from the bottom, a thin gold rule.
# 800 ms dissolve, gentle 6 px rise, text follows 150 ms. The glass plate
# variant needs true backdrop blur (P9) and ships when P9 is on main.
def worship():
    SERIF, SANS = 'Cormorant Garamond', 'Montserrat'
    p = 'worship-grace'
    name_f = field('name', 'Pastor Grace Mwangi', True)
    title_f = field('title', 'Lead Pastor', max_length=512)
    soft_in = {'type': 'fade', 'durationMs': 800, 'easing': 'ease-in-out'}
    soft_out = {'type': 'fade', 'durationMs': 560, 'easing': 'ease-in-out'}

    def rise(n, start):
        return animate(n, motion('rise', start, 800, 'ease-in-out', distance=6), motion('fade', 0, 400, 'ease-in-out'))

    def wash(w, h):
        return rect('wash', 0, 0, w, h, {'type': 'linear', 'angle': 180, 'stops': [
            {'color': tok('surface'), 'offset': 0}, {'color': tok('primary'), 'offset': 0.55}, {'color': tok('primary'), 'offset': 1}]})

    lt1 = template(p, 'lower-third', 'Worship Grace lower third', 'lower-third', 1200, 140, [name_f], [
        wash(1200, 140),
        rise(rect('rule', 80, 104, 360, 2, tok('accent')), 0),
        rise(text('name-text', 80, 30, 1040, 70, fld('name'), SERIF, 56, 600, tok('onPrimary')), 150)], LOWER_LEFT, soft_in, soft_out)
    lt2 = template(p, 'lower-third-2-line', 'Worship Grace two-line', 'lower-third', 1200, 170, [name_f, title_f], [
        wash(1200, 170),
        rise(text('name-text', 80, 20, 1040, 70, fld('name'), SERIF, 56, 600, tok('onPrimary')), 150),
        rise(rect('rule', 80, 96, 360, 2, tok('accent')), 0),
        rise(text('title-text', 80, 110, 1040, 38, fld('title'), SANS, 26, 400, tok('onPrimaryMuted'), textTransform='uppercase',
                  letterSpacing=0.12), 300)], LOWER_LEFT, soft_in, soft_out)
    tag = template(p, 'name-tag', 'Worship Grace name with ministry', 'lower-third', 1200, 170,
                   [name_f, title_f, field('tag', 'Youth Ministry', max_length=48)], [
        wash(1200, 170),
        rise(text('tag-text', 80, 12, 600, 34, fld('tag'), SANS, 22, 500, tok('accent'), textTransform='uppercase', letterSpacing=0.16), 0),
        rise(text('name-text', 80, 44, 1040, 70, fld('name'), SERIF, 56, 600, tok('onPrimary')), 150),
        rise(rect('rule', 80, 118, 360, 2, tok('accent')), 0),
        rise(text('title-text', 80, 128, 1040, 36, fld('title'), SANS, 24, 400, tok('onPrimaryMuted')), 300)], LOWER_LEFT, soft_in, soft_out)
    title = template(p, 'title', 'Worship Grace full-width title', 'title', 1920, 300,
                     [field('heading', 'Sunday Gathering', True), field('subtitle', 'Welcome home', max_length=512)], [
        wash(1920, 300),
        rise(text('heading-text', 160, 70, 1600, 110, fld('heading'), SERIF, 92, 600, tok('onPrimary'), align='center'), 150),
        rise(rect('rule', 860, 192, 200, 2, tok('accent')), 0),
        rise(text('subtitle-text', 160, 208, 1600, 56, fld('subtitle'), SERIF, 40, 400, tok('onPrimaryMuted'), align='center',
                  fontStyle='italic'), 300)], BOTTOM, soft_in, soft_out)
    bug = template(p, 'bug', 'Worship Grace corner bug', 'bug', 360, 84, [field('channel', 'Grace Church', True, 48)], [
        rect('rule', 0, 70, 120, 2, tok('accent')),
        text('channel-text', 0, 10, 360, 56, fld('channel'), SERIF, 44, 600, tok('onPrimary'),
             shadow={'color': tok('primary'), 'x': 0, 'y': 2, 'blur': 6})], UPPER_RIGHT, soft_in, soft_out)
    scripture = template(p, 'scripture', 'Worship Grace scripture', 'title', 1600, 360,
                         [field('verse', 'The Lord is my shepherd; I shall not want. He makes me lie down in green pastures. He leads me beside still waters.', True, 1024),
                          field('reference', 'Psalm 23:1-2', max_length=64)], [
        rect('wash', 0, 0, 1600, 360, {'type': 'radial', 'stops': [{'color': tok('primary'), 'offset': 0},
            {'color': tok('primary'), 'offset': 0.6}, {'color': tok('surface'), 'offset': 1}]}),
        rise(text('verse-text', 120, 48, 1360, 220, fld('verse'), SERIF, 52, 400, tok('onPrimary'), align='center',
                  maxLines=3, overflow='wrap-then-ellipsis', lineHeight=1.3, stagger={'by': 'line', 'ms': 150}), 150),
        rise(rect('rule', 740, 286, 120, 2, tok('accent')), 0),
        rise(text('reference-text', 120, 298, 1360, 40, fld('reference'), SANS, 26, 500, tok('accent'), align='center',
                  textTransform='uppercase', letterSpacing=0.16), 300)], CENTER, soft_in, soft_out)
    lyrics = template(p, 'lyrics', 'Worship Grace lyrics', 'title', 1600, 200,
                      [field('line-1', 'How great is our God, sing with me', True, 256), field('line-2', 'How great is our God, all will see', max_length=256)], [
        rise(text('line-1-text', 0, 20, 1600, 76, fld('line-1'), SERIF, 60, 600, tok('onPrimary'), align='center',
                  shadow={'color': tok('primary'), 'x': 0, 'y': 3, 'blur': 8}), 0),
        rise(text('line-2-text', 0, 104, 1600, 76, fld('line-2'), SERIF, 60, 600, tok('onPrimary'), align='center',
                  shadow={'color': tok('primary'), 'x': 0, 'y': 3, 'blur': 8}, collapseWhenEmpty=True), 150)], LOWER_CENTER, soft_in, soft_out)
    glass = template(p, 'glass-lower-third', 'Worship Grace glass lower third', 'lower-third', 1100, 170, [name_f, title_f], [
        # The glass: Program blurred behind a rounded plate whose own fill is the
        # palette surface (clear in the stock palettes), then a light veil of the
        # primary for text contrast. Opacity on the plate would dim the glass too.
        rect('glass', 0, 0, 1100, 170, tok('surface'), cornerRadius=16, backdrop={'blur': 24}),
        rect('veil', 0, 0, 1100, 170, tok('primary'), cornerRadius=16, opacity=0.4),
        rise(text('name-text', 48, 22, 1004, 70, fld('name'), SERIF, 56, 600, tok('onPrimary')), 150),
        rise(rect('rule', 48, 100, 300, 2, tok('accent')), 0),
        rise(text('title-text', 48, 114, 1004, 38, fld('title'), SANS, 26, 400, tok('onPrimaryMuted'), textTransform='uppercase',
                  letterSpacing=0.12), 300)], LOWER_LEFT, soft_in, soft_out)
    pack(p, 'Worship: Grace', 'worship',
         [{'family': SERIF, 'fallback': 'Georgia', 'files': ['CormorantGaramond-Regular.ttf', 'CormorantGaramond-Italic.ttf', 'CormorantGaramond-SemiBold.ttf']},
          {'family': SANS, 'files': ['Montserrat-Regular.ttf', 'Montserrat-Medium.ttf']}],
         {'warm': palette('Warm', '#2B1A12E6', '#F3E6D3FF', '#D9A94EFF', '#2B1A1200', '#FFF8EEFF', '#E8D9C3FF', '#2B1A12FF', '#2B1A12FF'),
          'midnight': palette('Midnight', '#0B1230E6', '#DCE3F5FF', '#C9B37AFF', '#0B123000', '#FFFFFFFF', '#C9D2EAFF', '#0B1230FF', '#0B1230FF'),
          'light': palette('Light', '#FBF8F2F2', '#3A3026FF', '#A5782BFF', '#FBF8F200', '#2A221BFF', '#5C5045FF', '#FBF8F2FF', '#FFFFFFFF')},
         [lt1, lt2, tag, title, bug, scripture, lyrics, glass])


# ── Corporate: Clarity ──────────────────────────────────────────────────────
# Rounded 8 px plate, brand-primary accent strip, soft shadow. Slide + fade
# 450 ms (ease-out stands in for quint-out until slice 8 names the curves).
def corporate():
    F = 'Montserrat'
    p = 'corporate-clarity'
    name_f = field('name', 'Ada Otieno', True)
    title_f = field('title', 'Head of Engineering, Northwind', max_length=512)
    soft = {'color': tok('surface'), 'dx': 0, 'dy': 8, 'blur': 24, 'spread': 0}
    slide_in = {'type': 'slide', 'direction': 'left', 'durationMs': 450, 'easing': 'ease-out'}
    fade_out = {'type': 'fade', 'durationMs': 300, 'easing': 'ease-in'}

    def plate(w, h):
        return rect('plate', 0, 0, w, h, tok('primary'), cornerRadius=8, shadow=soft)

    def follow(n, start, build=True):
        if build and n.get('overflow') != 'wrap-then-ellipsis':
            n['stagger'] = {'by': 'word', 'ms': 80}
        return animate(n, motion('rise', start, 300, 'quint-out', distance=8), motion('fade', 0, 200, 'ease-in'))

    lt1 = template(p, 'lower-third', 'Corporate Clarity lower third', 'lower-third', 980, 96, [name_f], [
        plate(980, 96), rect('strip', 24, 22, 6, 52, tok('accent')),
        follow(text('name-text', 52, 20, 900, 56, fld('name'), F, 42, 600, tok('onPrimary')), 150)], LOWER_LEFT, slide_in, fade_out)
    lt2 = template(p, 'lower-third-2-line', 'Corporate Clarity two-line', 'lower-third', 980, 130, [name_f, title_f], [
        plate(980, 130), rect('strip', 24, 22, 6, 86, tok('accent')),
        follow(text('name-text', 52, 16, 900, 56, fld('name'), F, 42, 600, tok('onPrimary')), 150),
        follow(text('title-text', 52, 74, 900, 38, fld('title'), F, 26, 400, tok('onPrimaryMuted'), collapseWhenEmpty=True), 230)],
        LOWER_LEFT, slide_in, fade_out)
    tag = template(p, 'name-tag', 'Corporate Clarity name with tag', 'lower-third', 980, 130,
                   [name_f, title_f, field('tag', 'Speaker', max_length=32)], [
        plate(980, 130), rect('strip', 24, 22, 6, 86, tok('accent')),
        rect('tag-plate', 820, 22, 136, 36, tok('accent'), cornerRadius=18),
        text('tag-text', 820, 25, 136, 30, fld('tag'), F, 20, 600, tok('onAccent'), align='center', textTransform='uppercase', letterSpacing=0.08),
        follow(text('name-text', 52, 16, 750, 56, fld('name'), F, 42, 600, tok('onPrimary')), 150),
        follow(text('title-text', 52, 74, 900, 38, fld('title'), F, 26, 400, tok('onPrimaryMuted')), 230)], LOWER_LEFT, slide_in, fade_out)
    title = template(p, 'title', 'Corporate Clarity full-width title', 'title', 1600, 220,
                     [field('heading', 'Quarterly All-Hands', True), field('subtitle', 'Product, people and the year ahead', max_length=512)], [
        rect('plate', 0, 0, 1600, 220, tok('primary'), cornerRadius=8, shadow=soft),
        rect('strip', 0, 0, 1600, 8, tok('accent'), cornerRadius=4),
        follow(text('heading-text', 80, 42, 1440, 84, fld('heading'), F, 64, 600, tok('onPrimary'), align='center'), 150),
        follow(text('subtitle-text', 80, 134, 1440, 48, fld('subtitle'), F, 32, 400, tok('onPrimaryMuted'), align='center'), 230)],
        CENTER, {'type': 'fade', 'durationMs': 450, 'easing': 'ease-out'}, fade_out)
    bug = template(p, 'bug', 'Corporate Clarity corner bug', 'bug', 280, 72, [field('channel', 'Northwind', True, 32)], [
        rect('plate', 0, 0, 280, 72, tok('primary'), cornerRadius=8, shadow=soft),
        rect('strip', 16, 18, 6, 36, tok('accent')),
        text('channel-text', 36, 14, 230, 44, fld('channel'), F, 30, 600, tok('onPrimary'))], UPPER_RIGHT,
        {'type': 'fade', 'durationMs': 450, 'easing': 'ease-out'}, fade_out)
    webinar = template(p, 'webinar-title', 'Corporate Clarity webinar title card', 'title', 1400, 420,
                       [field('kicker', 'Webinar', max_length=48), field('heading', 'Scaling Support Without Burnout', True),
                        field('presenter', 'Ada Otieno, Head of Engineering', max_length=256), field('date', 'Thursday 4 December, 16:00 GMT', max_length=96)], [
        rect('plate', 0, 0, 1400, 420, tok('primary'), cornerRadius=8, shadow=soft),
        rect('band', 0, 0, 1400, 420, {'type': 'linear', 'angle': 135, 'stops': [tok('accent'), tok('primary'), tok('primary')]}, cornerRadius=8, opacity=0.25),
        # The pill keeps a margin round the text box: a text region is judged
        # with its box grown by 0.35 x the size, which must stay on the pill.
        rect('kicker-plate', 80, 56, 180, 52, tok('accent'), cornerRadius=26),
        text('kicker-text', 96, 66, 148, 32, fld('kicker'), F, 20, 600, tok('onAccent'), align='center', textTransform='uppercase', letterSpacing=0.1),
        follow(text('heading-text', 80, 128, 1240, 160, fld('heading'), F, 60, 600, tok('onPrimary'), maxLines=2,
                    overflow='wrap-then-ellipsis', lineHeight=1.15), 150),
        follow(text('presenter-text', 80, 304, 1240, 44, fld('presenter'), F, 30, 500, tok('onPrimary')), 230),
        follow(text('date-text', 80, 352, 1240, 40, fld('date'), F, 26, 400, tok('onPrimaryMuted')), 310)], CENTER,
        {'type': 'fade', 'durationMs': 450, 'easing': 'ease-out'}, fade_out)
    company = template(p, 'speaker-company', 'Corporate Clarity speaker with company', 'lower-third', 1100, 130,
                       [name_f, field('role', 'Head of Engineering', max_length=256), field('company', 'NORTHWIND', True, 64)], [
        plate(1100, 130),
        rect('company-plate', 0, 0, 300, 130, tok('secondary'), cornerRadius=8),
        text('company-text', 20, 44, 260, 42, fld('company'), F, 30, 600, tok('onSecondary'), align='center', textTransform='uppercase',
             letterSpacing=0.06),
        rect('strip', 324, 22, 6, 86, tok('accent')),
        follow(text('name-text', 352, 16, 720, 56, fld('name'), F, 42, 600, tok('onPrimary')), 150),
        follow(text('role-text', 352, 74, 720, 38, fld('role'), F, 26, 400, tok('onPrimaryMuted')), 230)], LOWER_LEFT, slide_in, fade_out)
    glass = template(p, 'glass-lower-third', 'Corporate Clarity glass lower third', 'lower-third', 980, 130, [name_f, title_f], [
        rect('glass', 0, 0, 980, 130, tok('surface'), cornerRadius=8, backdrop={'blur': 32}),
        rect('veil', 0, 0, 980, 130, tok('primary'), cornerRadius=8, opacity=0.55),
        rect('strip', 24, 22, 6, 86, tok('accent')),
        follow(text('name-text', 52, 16, 900, 56, fld('name'), F, 42, 600, tok('onPrimary')), 150),
        follow(text('title-text', 52, 74, 900, 38, fld('title'), F, 26, 400, tok('onPrimaryMuted'), collapseWhenEmpty=True), 230)],
        LOWER_LEFT, slide_in, fade_out)
    pack(p, 'Corporate: Clarity', 'corporate',
         [{'family': F, 'files': ['Montserrat-Regular.ttf', 'Montserrat-Medium.ttf', 'Montserrat-SemiBold.ttf']}],
         {'clarity-blue': palette('Clarity Blue', '#FFFFFFFF', '#1F4E8CFF', '#1F6FEBFF', '#0B1F3A40', '#14213DFF', '#5A6578FF', '#FFFFFFFF', '#FFFFFFFF'),
          'graphite': palette('Graphite', '#22262EFF', '#3B82F6FF', '#3B82F6FF', '#00000066', '#FFFFFFFF', '#AEB6C4FF', '#FFFFFFFF', '#FFFFFFFF'),
          'forest': palette('Forest', '#F7F8F5FF', '#1E5B3AFF', '#2E8B57FF', '#0F2A1A40', '#16241BFF', '#55635AFF', '#FFFFFFFF', '#FFFFFFFF')},
         [lt1, lt2, tag, title, bug, webinar, company, glass])


# ── Esports: Neon ───────────────────────────────────────────────────────────
# Angular HUD frame (polygon with cut corners) on near-black, accent glow,
# scanlines. 250 ms stepped in (three opacity steps as staggered cuts).
def esports():
    F = 'Chakra Petch'
    p = 'esports-neon'
    name_f = field('name', 'NOVA', True, 64)
    title_f = field('title', 'Team Vertex  |  Entry fragger', max_length=512)
    glow = {'color': tok('accent'), 'blur': 14, 'spread': 1}

    def hud(ident, x, y, w, h, fill, cut=0.12, shadow=None):
        cx, cy = cut * h / w, cut
        pts = [[cx * 2, 0], [1, 0], [1, 1 - cy * 2], [1 - cx * 2, 1], [0, 1], [0, cy * 2]]
        pts = [[round(a, 4), round(b, 4)] for a, b in pts]
        n = rect(ident, x, y, w, h, fill, shape='polygon', points=pts)
        if shadow:
            n['shadow'] = shadow
        return n

    def glitch(n, start):
        return animate(n, motion('cut', start, 0), motion('cut', 0, 0))

    def scanlines(x, y, w, h, count=6):
        step = h / count
        lines = [rect('scan-%d' % i, 0, round(i * step), w, 1, tok('onPrimary'), opacity=0.07) for i in range(count)]
        return {'id': 'scanlines', 'type': 'group', 'x': x, 'y': y, 'width': w, 'height': h, 'children': lines}

    def caps(ident, x, y, w, h, value, size, color, weight=700, **props):
        return text(ident, x, y, w, h, value, F, size, weight, color, textTransform=props.pop('textTransform', 'uppercase'),
                    letterSpacing=props.pop('letterSpacing', 0.08), **props)

    lt1 = template(p, 'lower-third', 'Esports Neon lower third', 'lower-third', 900, 96, [name_f], [
        glitch(hud('frame', 0, 0, 900, 96, tok('primary'), shadow=glow), 0),
        glitch(scanlines(10, 8, 880, 80), 80),
        glitch(dict(rect('accent-bar', 24, 20, 6, 56, tok('accent')), loop={'type': 'pulse', 'periodMs': 1600}), 160),
        glitch(caps('name-text', 50, 16, 820, 64, fld('name'), 52, tok('onPrimary')), 250)], LOWER_LEFT,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 180, 'easing': 'ease-in'})
    lt2 = template(p, 'lower-third-2-line', 'Esports Neon two-line', 'lower-third', 900, 140, [name_f, title_f], [
        glitch(hud('frame', 0, 0, 900, 140, tok('primary'), shadow=glow), 0),
        glitch(scanlines(10, 8, 880, 124, 8), 80),
        glitch(dict(rect('accent-bar', 24, 20, 6, 100, tok('accent')), loop={'type': 'pulse', 'periodMs': 1600}), 160),
        glitch(caps('name-text', 50, 14, 820, 64, fld('name'), 52, tok('onPrimary')), 250),
        glitch(caps('title-text', 50, 84, 820, 40, fld('title'), 28, tok('accent'), 600, letterSpacing=0.04), 250)], LOWER_LEFT,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 180, 'easing': 'ease-in'})
    tag = template(p, 'name-tag', 'Esports Neon name with role tag', 'lower-third', 900, 140,
                   [name_f, title_f, field('tag', 'MVP', max_length=16)], [
        glitch(hud('frame', 0, 0, 900, 140, tok('primary'), shadow=glow), 0),
        glitch(scanlines(10, 8, 880, 124, 8), 80),
        glitch(hud('tag-plate', 740, 18, 136, 44, tok('accent'), 0.2), 160),
        glitch(caps('tag-text', 740, 22, 136, 36, fld('tag'), 26, tok('onAccent'), align='center'), 250),
        glitch(caps('name-text', 30, 14, 700, 64, fld('name'), 52, tok('onPrimary')), 250),
        glitch(caps('title-text', 30, 84, 840, 40, fld('title'), 28, tok('accent'), 600, letterSpacing=0.04), 250)], LOWER_LEFT,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 180, 'easing': 'ease-in'})
    title = template(p, 'title', 'Esports Neon full-width title', 'title', 1800, 220,
                     [field('heading', 'Grand Final', True), field('subtitle', 'Vertex vs Halcyon  |  Best of five', max_length=512)], [
        glitch(hud('frame', 0, 0, 1800, 220, tok('primary'), 0.15, glow), 0),
        glitch(scanlines(20, 14, 1760, 192, 10), 80),
        glitch(caps('heading-text', 60, 30, 1680, 110, fld('heading'), 92, tok('onPrimary'), align='center', letterSpacing=0.12), 250),
        glitch(caps('subtitle-text', 60, 146, 1680, 48, fld('subtitle'), 32, tok('accent'), 600, align='center'), 250)], CENTER,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 180, 'easing': 'ease-in'})
    bug = template(p, 'bug', 'Esports Neon corner bug', 'bug', 260, 76, [field('channel', 'LIVE', True, 24)], [
        hud('frame', 0, 0, 260, 76, tok('primary'), 0.2, glow),
        dict(rect('pulse', 24, 30, 16, 16, tok('accent')), loop={'type': 'pulse', 'periodMs': 1200}),
        caps('channel-text', 52, 12, 190, 52, fld('channel'), 40, tok('onPrimary'))], UPPER_RIGHT)
    player = template(p, 'player-card', 'Esports Neon player card', 'lower-third', 760, 300,
                      [name_f, field('team', 'Team Vertex', max_length=64), field('role', 'Entry fragger', max_length=64),
                       field('stat-1', '1.32', max_length=8), field('label-1', 'Rating', max_length=16),
                       field('stat-2', '87.4', max_length=8), field('label-2', 'ADR', max_length=16),
                       field('stat-3', '62%', max_length=8), field('label-3', 'HS', max_length=16)], [
        glitch(hud('frame', 0, 0, 760, 300, tok('primary'), 0.08, glow), 0),
        glitch(scanlines(12, 10, 736, 280, 10), 80),
        glitch(rect('header', 30, 30, 700, 2, tok('accent')), 160),
        glitch(caps('team-text', 30, 40, 700, 34, fld('team'), 24, tok('accent'), 600), 250),
        glitch(caps('name-text', 30, 74, 700, 80, fld('name'), 64, tok('onPrimary'), letterSpacing=0.1), 250),
        glitch(caps('role-text', 30, 152, 700, 34, fld('role'), 24, tok('onPrimaryMuted'), 600, letterSpacing=0.04), 250),
        glitch(caps('stat-1-text', 30, 200, 220, 56, fld('stat-1'), 46, tok('onPrimary')), 250),
        glitch(caps('label-1-text', 30, 254, 220, 30, fld('label-1'), 20, tok('accent'), 600), 250),
        glitch(caps('stat-2-text', 270, 200, 220, 56, fld('stat-2'), 46, tok('onPrimary')), 250),
        glitch(caps('label-2-text', 270, 254, 220, 30, fld('label-2'), 20, tok('accent'), 600), 250),
        glitch(caps('stat-3-text', 510, 200, 220, 56, fld('stat-3'), 46, tok('onPrimary')), 250),
        glitch(caps('label-3-text', 510, 254, 220, 30, fld('label-3'), 20, tok('accent'), 600), 250)], LOWER_LEFT,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 180, 'easing': 'ease-in'})
    casters = template(p, 'caster-duo', 'Esports Neon caster duo', 'lower-third', 1500, 120,
                       [field('caster-1', 'SABLE', True, 64), field('handle-1', '@sablecasts', max_length=64),
                        field('caster-2', 'RIOT GRRL', max_length=64), field('handle-2', '@riotgrrl', max_length=64)], [
        glitch(hud('frame-1', 0, 0, 720, 120, tok('primary'), 0.15, glow), 0),
        glitch(hud('frame-2', 780, 0, 720, 120, tok('primary'), 0.15, glow), 60),
        glitch(rect('bar-1', 24, 22, 6, 76, tok('accent')), 160),
        glitch(rect('bar-2', 804, 22, 6, 76, tok('accent')), 160),
        glitch(caps('caster-1-text', 50, 12, 640, 60, fld('caster-1'), 48, tok('onPrimary')), 250),
        glitch(caps('handle-1-text', 50, 70, 640, 36, fld('handle-1'), 26, tok('accent'), 600, textTransform='none', letterSpacing=0.02), 250),
        glitch(caps('caster-2-text', 830, 12, 640, 60, fld('caster-2'), 48, tok('onPrimary')), 250),
        glitch(caps('handle-2-text', 830, 70, 640, 36, fld('handle-2'), 26, tok('accent'), 600, textTransform='none', letterSpacing=0.02), 250)],
        LOWER_CENTER, {'type': 'none'}, {'type': 'fade', 'durationMs': 180, 'easing': 'ease-in'})
    pack(p, 'Esports: Neon', 'esports',
         [{'family': F, 'files': ['ChakraPetch-Regular.ttf', 'ChakraPetch-SemiBold.ttf', 'ChakraPetch-Bold.ttf']}],
         {'neon-cyan': palette('Neon Cyan', '#0A0D14F2', '#141A26FF', '#22E5FFFF', '#05070BFF', '#F2F7FFFF', '#9AA7BDFF', '#F2F7FFFF', '#05070BFF'),
          'toxic': palette('Toxic Green', '#0A0F0BF2', '#141C16FF', '#7CFF4FFF', '#050805FF', '#F3FFF0FF', '#9DB59AFF', '#F3FFF0FF', '#050805FF'),
          'magenta': palette('Magenta', '#120A14F2', '#1E1424FF', '#FF3DCBFF', '#08050AFF', '#FFF0FBFF', '#B79AB3FF', '#FFF0FBFF', '#08050AFF')},
         [lt1, lt2, tag, title, bug, player, casters])


# ── Talk: Studio ────────────────────────────────────────────────────────────
# Two-tone pills (name pill + role pill), handle line. Pop 400 ms (back-out
# arrives with slice 8), pills staggered 90 ms.
def talk():
    F = 'Poppins'
    p = 'talk-studio'
    name_f = field('name', 'Ada Otieno', True)
    title_f = field('title', 'Author, The Long Signal', max_length=512)

    def pop(n, start):
        return animate(n, motion('pop', start, 400), motion('fade', 0, 200, 'ease-in'))

    def pill(ident, x, y, w, h, fill):
        return rect(ident, x, y, w, h, fill, cornerRadius=h / 2)

    lt1 = template(p, 'lower-third', 'Talk Studio lower third', 'lower-third', 900, 84, [name_f], [
        pop(pill('name-pill', 0, 0, 900, 84, tok('primary')), 0),
        pop(text('name-text', 40, 14, 820, 56, fld('name'), F, 40, 600, tok('onPrimary')), 90)], LOWER_LEFT,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 200, 'easing': 'ease-in'})
    lt2 = template(p, 'lower-third-2-line', 'Talk Studio two-line', 'lower-third', 900, 140, [name_f, title_f], [
        pop(pill('name-pill', 0, 0, 900, 84, tok('primary')), 0),
        pop(pill('role-pill', 40, 88, 640, 52, tok('secondary')), 90),
        pop(text('name-text', 40, 14, 820, 56, fld('name'), F, 40, 600, tok('onPrimary')), 90),
        pop(text('title-text', 70, 96, 590, 36, fld('title'), F, 24, 500, tok('onSecondary')), 180)], LOWER_LEFT,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 200, 'easing': 'ease-in'})
    handle = template(p, 'name-handle', 'Talk Studio guest with handle', 'lower-third', 900, 140,
                      [name_f, title_f, field('handle', '@adaotieno', max_length=64)], [
        pop(pill('name-pill', 0, 0, 900, 84, tok('primary')), 0),
        pop(pill('role-pill', 40, 88, 520, 52, tok('secondary')), 90),
        pop(pill('handle-pill', 570, 88, 290, 52, tok('accent')), 180),
        pop(text('name-text', 40, 14, 820, 56, fld('name'), F, 40, 600, tok('onPrimary')), 90),
        pop(text('title-text', 70, 96, 470, 36, fld('title'), F, 24, 500, tok('onSecondary')), 180),
        pop(text('handle-text', 590, 96, 250, 36, fld('handle'), F, 24, 600, tok('onAccent'), align='center'), 270)], LOWER_LEFT,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 200, 'easing': 'ease-in'})
    title = template(p, 'title', 'Talk Studio full-width title', 'title', 1600, 240,
                     [field('heading', 'The Late Conversation', True), field('subtitle', 'Tonight: stories that stay with you', max_length=512)], [
        pop(pill('plate', 0, 0, 1600, 150, tok('primary')), 0),
        pop(pill('sub-pill', 300, 160, 1000, 72, tok('secondary')), 90),
        pop(text('heading-text', 80, 30, 1440, 90, fld('heading'), F, 68, 600, tok('onPrimary'), align='center'), 90),
        pop(text('subtitle-text', 320, 174, 960, 44, fld('subtitle'), F, 30, 500, tok('onSecondary'), align='center'), 180)], CENTER,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 200, 'easing': 'ease-in'})
    bug = template(p, 'bug', 'Talk Studio corner bug', 'bug', 300, 72, [field('channel', 'Studio Live', True, 32)], [
        pill('plate', 0, 0, 300, 72, tok('primary')),
        pill('dot', 18, 22, 28, 28, tok('accent')),
        text('channel-text', 56, 12, 230, 48, fld('channel'), F, 30, 600, tok('onPrimary'))], UPPER_RIGHT)
    next_up = template(p, 'up-next', 'Talk Studio up next', 'lower-third', 1000, 150,
                       [field('label', 'Up next', max_length=32), field('name', 'Dr. Lena Park', True), field('topic', 'Why sleep is the new diet', max_length=256)], [
        pop(pill('label-pill', 0, 0, 220, 56, tok('accent')), 0),
        pop(pill('name-pill', 0, 62, 1000, 88, tok('primary')), 90),
        pop(text('label-text', 0, 10, 220, 36, fld('label'), F, 24, 600, tok('onAccent'), align='center', textTransform='uppercase',
                 letterSpacing=0.08), 90),
        pop(text('name-text', 40, 70, 560, 56, fld('name'), F, 38, 600, tok('onPrimary')), 180),
        pop(text('topic-text', 600, 82, 370, 40, fld('topic'), F, 24, 400, tok('onPrimaryMuted'), align='right'), 180)], LOWER_LEFT,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 200, 'easing': 'ease-in'})
    pack(p, 'Talk: Studio', 'talk',
         [{'family': F, 'files': ['Poppins-Regular.ttf', 'Poppins-Medium.ttf', 'Poppins-SemiBold.ttf']}],
         {'studio-coral': palette('Studio Coral', '#1E1B2EFF', '#FF6F59FF', '#FFD166FF', '#1E1B2EB3', '#FFFFFFFF', '#C9C5DAFF', '#FFFFFFFF', '#1E1B2EFF'),
          'mint': palette('Mint', '#0E2A2AFF', '#2EC4B6FF', '#CBF3F0FF', '#0E2A2AB3', '#FFFFFFFF', '#B8D6D3FF', '#0E2A2AFF', '#0E2A2AFF'),
          'mono': palette('Mono', '#111111FF', '#F2F2F2FF', '#FFFFFFFF', '#111111B3', '#FFFFFFFF', '#BDBDBDFF', '#111111FF', '#111111FF')},
         [lt1, lt2, handle, title, bug, next_up])


# ── Minimal: Line ───────────────────────────────────────────────────────────
# No plate: a hairline rule and large weight contrast, wide tracking on the
# title. A wipe stands in for the text mask reveal until slice 8 adds it.
def minimal():
    F = 'Inter'
    p = 'minimal-line'
    name_f = field('name', 'Ada Otieno', True)
    title_f = field('title', 'Head of Engineering', max_length=512)
    shade = {'color': tok('surface'), 'x': 0, 'y': 2, 'blur': 6}

    def reveal(n, start):
        return animate(n, motion('reveal', start, 500, 'quint-out', direction='left', feather=64),
                       motion('reveal', 0, 350, 'ease-in', direction='left', feather=64))

    lt1 = template(p, 'lower-third', 'Minimal Line single-line name', 'lower-third', 1000, 90, [name_f], [
        reveal(rect('rule', 0, 84, 480, 2, tok('accent')), 0),
        reveal(text('name-text', 0, 10, 1000, 66, fld('name'), F, 52, 700, tok('onPrimary'), letterSpacing=-0.01, shadow=shade), 100)],
        LOWER_LEFT, {'type': 'none'}, {'type': 'fade', 'durationMs': 200, 'easing': 'ease-in'})
    lt2 = template(p, 'lower-third-2-line', 'Minimal Line two-line', 'lower-third', 1000, 130, [name_f, title_f], [
        reveal(text('name-text', 0, 0, 1000, 66, fld('name'), F, 52, 700, tok('onPrimary'), letterSpacing=-0.01, shadow=shade), 100),
        reveal(rect('rule', 0, 76, 480, 2, tok('accent')), 0),
        reveal(text('title-text', 0, 88, 1000, 36, fld('title'), F, 22, 400, tok('onPrimaryMuted'), textTransform='uppercase',
                    letterSpacing=0.24, shadow=shade), 200)], LOWER_LEFT,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 200, 'easing': 'ease-in'})
    tag = template(p, 'name-tag', 'Minimal Line name with tag', 'lower-third', 1000, 130,
                   [name_f, title_f, field('tag', 'Guest', max_length=24)], [
        reveal(text('tag-text', 0, 0, 400, 30, fld('tag'), F, 18, 700, tok('accent'), textTransform='uppercase', letterSpacing=0.3, shadow=shade), 0),
        reveal(text('name-text', 0, 26, 1000, 66, fld('name'), F, 52, 700, tok('onPrimary'), letterSpacing=-0.01, shadow=shade), 100),
        reveal(rect('rule', 0, 98, 480, 2, tok('accent')), 0),
        reveal(text('title-text', 0, 106, 1000, 24, fld('title'), F, 18, 400, tok('onPrimaryMuted'), textTransform='uppercase',
                    letterSpacing=0.24, shadow=shade), 200)], LOWER_LEFT,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 200, 'easing': 'ease-in'})
    title = template(p, 'title', 'Minimal Line full-width title', 'title', 1600, 220,
                     [field('heading', 'In Conversation', True), field('subtitle', 'Season four, episode two', max_length=512)], [
        reveal(text('heading-text', 0, 20, 1600, 110, fld('heading'), F, 92, 700, tok('onPrimary'), align='center', letterSpacing=-0.02,
                    shadow=shade), 100),
        reveal(rect('rule', 640, 146, 320, 2, tok('accent')), 0),
        reveal(text('subtitle-text', 0, 164, 1600, 40, fld('subtitle'), F, 24, 400, tok('onPrimaryMuted'), align='center',
                    textTransform='uppercase', letterSpacing=0.3, shadow=shade), 200)], CENTER,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 200, 'easing': 'ease-in'})
    bug = template(p, 'bug', 'Minimal Line corner bug', 'bug', 260, 70, [field('channel', 'Line', True, 24)], [
        text('channel-text', 0, 4, 260, 52, fld('channel'), F, 40, 700, tok('onPrimary'), align='right', shadow=shade),
        rect('rule', 140, 62, 120, 2, tok('accent'))], UPPER_RIGHT)
    quote = template(p, 'quote', 'Minimal Line quote', 'title', 1400, 340,
                     [field('quote', 'Simplicity is not the absence of clutter; it is the presence of clarity.', True, 1024),
                      field('attribution', 'Ada Otieno', max_length=128)], [
        reveal(rect('rule', 0, 0, 2, 340, tok('accent')), 0),
        reveal(text('quote-text', 48, 10, 1340, 230, fld('quote'), F, 48, 400, tok('onPrimary'), maxLines=3,
                    overflow='wrap-then-ellipsis', lineHeight=1.3, fontStyle='italic', shadow=shade), 100),
        reveal(text('attribution-text', 48, 274, 1340, 40, fld('attribution'), F, 22, 700, tok('onPrimaryMuted'),
                    textTransform='uppercase', letterSpacing=0.3, shadow=shade), 200)], CENTER,
        {'type': 'none'}, {'type': 'fade', 'durationMs': 200, 'easing': 'ease-in'})
    pack(p, 'Minimal: Line', 'minimal',
         [{'family': F, 'files': ['Inter-Regular.ttf', 'Inter-Italic.ttf', 'Inter-Bold.ttf']}],
         {'ink': palette('Ink', '#00000000', '#FFFFFFFF', '#FFFFFFFF', '#00000099', '#FFFFFFFF', '#D9D9D9FF', '#111111FF', '#111111FF'),
          'paper': palette('Paper', '#00000000', '#111111FF', '#111111FF', '#FFFFFF99', '#111111FF', '#4D4D4DFF', '#FFFFFFFF', '#FFFFFFFF'),
          'gold': palette('Gold', '#00000000', '#E0B44CFF', '#E0B44CFF', '#00000099', '#FFFFFFFF', '#E8DCC0FF', '#111111FF', '#111111FF')},
         [lt1, lt2, tag, title, bug, quote])


FACES = {'Regular': (400, 'normal'), 'Italic': (400, 'italic'), 'Medium': (500, 'normal'), 'SemiBold': (600, 'normal'),
         'SemiBoldItalic': (600, 'italic'), 'Bold': (700, 'normal'), 'BoldItalic': (700, 'italic')}

# Files slice 7 shipped by hand in news-signal: kept as they are, listed first.
HAND_WRITTEN = {'news-signal': ['templates/anchor.json']}

STATUS = {}


def font_entries(groups):
    out = []
    for g in groups:
        for name in g['files']:
            weight, style = FACES[name.rsplit('-', 1)[1].rsplit('.', 1)[0]]
            out.append({'family': g['family'], 'file': 'fonts/' + name, 'weight': weight, 'style': style,
                        'fallback': g.get('fallback', 'Segoe UI')})
    return out


def write(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(json.dumps(value, indent=1, ensure_ascii=False) + '\n')


def main():
    for build in (news, sports, worship, corporate, esports, talk, minimal):
        build()
    for ident, p in PACKS.items():
        base = os.path.join(OUT, ident)
        keep = set(HAND_WRITTEN.get(ident, []))
        for folder in ('palettes', 'templates'):
            directory = os.path.join(base, folder)
            if os.path.isdir(directory):
                for name in os.listdir(directory):
                    if folder + '/' + name not in keep:
                        os.remove(os.path.join(directory, name))
        for name, value in p['palettes'].items():
            write(os.path.join(base, 'palettes', name + '.json'), value)
        for slug, doc in p['templates']:
            write(os.path.join(base, 'templates', slug + '.json'), doc)
        write(os.path.join(base, 'pack.json'), {
            'id': ident, 'name': p['name'], 'genre': p['genre'], 'version': '1.0.0',
            'palettes': ['palettes/' + n + '.json' for n in p['palettes']],
            'fonts': font_entries(p['fonts']),
            'templates': HAND_WRITTEN.get(ident, []) + ['templates/' + slug + '.json' for slug, _ in p['templates']],
            'metadata': {'status': STATUS.get(ident, 'Wave 1 (graphics styles slice 10).'),
                         'defaultPalette': 'palettes/' + next(iter(p['palettes'])) + '.json',
                         'generator': 'scripts/graphics/generate-style-packs.py'},
        })
        print(ident, len(p['templates']), 'templates', len(p['palettes']), 'palettes')


if __name__ == '__main__':
    sys.exit(main())
