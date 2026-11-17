#!/usr/bin/env python3
"""Generates packages/TearsOfTheKingdom/dualscreen/manifest.json: the Tears of the Kingdom pages.

The owner's rule: the Tears of the Kingdom page is Breath of the Wild's Eden Duo Ready page, widget for widget, with
Tears of the Kingdom's values. So this script does not draw a page of its own. It reads Breath of the Wild's package
(packages/BreathOfTheWild/dualscreen/manifest.json) and rewrites its Ready, main-menu and Loading pages with one table:

  - every Ready widget is kept, rewritten or dropped by its index (READY_DROP, with the reason for each range);
  - bound names: botw.X becomes totk.X (the module publishes Breath of the Wild's names with totk. in front), except
    the few in RENAME; names in CONST are folded into the expressions (the quick picker's state is always 0, ...);
  - pictures: Breath of the Wild's romfs textures become module:totk:lyt/<Tears of the Kingdom texture> (TEX), its
    StockItem icons module:totk:icon/<actor>; file: art is copied from the Breath of the Wild package with its
    provenance (all of it is Eden Duo-generated, none is game art);
  - identity (spec C04, C38, C60): the Sheikah blue becomes Tears of the Kingdom's teal, "Sheikah Slate" becomes
    "Purah Pad", the wrong-build cover asks for Tears of the Kingdom 1.4.3;
  - Breath of the Wild's module actions (quick picker, help, arrow cycling, weapon swap, quests) are dropped; pager and
    card switches become flag actions. The item actions come back as Tears of the Kingdom's own: the Inventory
    detail's action buttons and a quick picker on the three gear corners (QUICK), drawn in Breath of the Wild's style
    (its quick picker panel, grid cells and 520 px confirm bar; its Inventory's action bar). Their only module actions
    are "totk.act" (request the selected item's action) and "totk.select" (the picker's cell). The module offers them
    only on TotK Native's port; in Eden Duo every action shows disabled, "Item actions need the TotK Native app".

The build fails if a botw. name, a Breath of the Wild romfs path or a module action is left over, or if the page binds
a totk. name that the module neither publishes (PUBLISHED) nor is listed as not read yet (UNREAD).

The Inventory and Map pages are Tears of the Kingdom's own (Breath of the Wild's are built on its module's actions
and map data), drawn in Breath of the Wild's palette and type. Every picture is decoded at runtime from the player's
own romfs by the native module (native/modules/0100F2C0115B6000.cpp); the package ships no game art.

    gen_manifest.py [out]
"""
import copy
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '../..'))
BOTW_DIR = os.path.join(ROOT, 'packages/BreathOfTheWild/dualscreen')
OUT = os.path.join(ROOT, 'packages/TearsOfTheKingdom/dualscreen/manifest.json')
VERSION = '0.3.0'
BUILD_143 = '277178B7DBA1B6D4949A84778D4ABC58B31B34F5000000000000000000000000'   # 1.4.3 main's module ID
FONT = 'ninP_RodinNTLG-B_DH_003_subset:22'   # TotK's Normal_00 face = BotW's page font (Rodin NTLG B), ~30.4 px/em: cap height 22 px
M = 'module:totk:'
R = 'romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#'


def lyt(texture):
    return f'{M}lyt/{texture}'


# ---------------------------------------------------------------------------------------------------------------
# The rewrite table
# ---------------------------------------------------------------------------------------------------------------

def span(a, b):
    return set(range(a, b + 1))


# Ready widgets (Breath of the Wild's pages[ready].widgets index) that are dropped, and why. Everything else in
# 1-319, 572-592 and 808-863 is kept and rewritten; 320-571, 593-807 and 864-989 are the quick picker.
READY_DROP = [
    ({0}, 'fx_probe: Breath of the Wild\'s layout-animation loader'),
    (span(11, 30), 'map pins on the disc: no Tears of the Kingdom pin read yet'),
    (span(31, 45), 'quest marks on the disc: no quest read yet'),
    (span(48, 57), 'shrine panel (in a shrine): no in-shrine read yet'),
    ({63}, 'context caption under "Minimap unavailable": Breath of the Wild\'s context names'),
    ({67} | span(159, 163), 'quest page of the left pager: no quest read yet'),
    (span(70, 73), 'second and third buff rows: Tears of the Kingdom has one meal effect at a time'),
    (span(80, 82) | span(84, 86), 'sensor ping and plus glyphs: no sensor-target read yet (the resting glyph stays)'),
    ({93, 98, 107, 116, 122, 133}, 'corner and well taps: they open the quick picker or cycle gear (no actions)'),
    ({135, 136} | span(582, 586), 'nearest-shrine compass card: no nearest-shrine read yet'),
    ({137, 138, 139} | span(156, 158), 'temperature glows: no temperature read yet'),
    (span(141, 148), 'weather forecast page: the right pager carries Buffs and Food (spec C25)'),
    (span(152, 155), 'temperature dial halves and needle: no temperature read yet (the dial shows its empty state)'),
    ({164, 165}, 'area name and map-marker caption: no area read yet ("Area unavailable" shows, as Breath of the Wild)'),
    ({169}, 'left pager swipes: one left page (Sages) until quests are read'),
    (span(171, 176), 'three-dot right pager: at most two right pages (Buffs, Food)'),
    (span(190, 193), 'attack and defence numbers on the corner cards: not read yet'),
    ({194}, 'view cone: no camera read'),
    ({197}, 'secondary-weapon swap hint: Tears of the Kingdom has no weapon swap (the well carries Fuse)'),
    ({198, 199}, 'arrow up/down hints: arrow selection is not offered'),
    (span(200, 202) | span(205, 207), 'left pager dots: one left page'),
    (span(218, 220), 'Quest command: no quest page yet (Inventory and Map are centred, spec C28)'),
    (span(233, 238) | span(271, 291) | {293, 572}, 'clothing card: no temperature read yet'),
    ({244, 252, 260, 268}, 'food-card count texts: never shown by Breath of the Wild either'),
    ({297, 298, 299, 304, 305, 306, 316, 317, 318, 574, 575, 576},
     'cover frames: Breath of the Wild\'s conductor-frame art, which Tears of the Kingdom does not have'),
]
DROP = set().union(*(s for s, _ in READY_DROP))
KEEP_RANGES = span(1, 319) | span(572, 592) | span(808, 863)

# Breath of the Wild's Common layout textures -> Tears of the Kingdom's (Common.Product.140 timg/__Combined.bntx)
TEX = {
    'Gradation128_01^t': 'Gradation128_01^s',
    'Nt_MapPlayerIconNew_00^s': 'Nt_MapPlayerIconNew_00^s',
    'Nt_MapPlayerIconNewSh_00^s': 'Nt_MapPlayerIconNewSh_00^s',
    'Nt_RupeeIconNew_00^l': 'Nt_RupeeIconNew_00^w',
    'Nt_AppMapNorthMark_00^d': 'Nt_AppMapNorthMark_00^d',
    'Nt_Circle64_00^s': 'Nt_Circle64_00^s',
    'Mask_Glow^s': 'Mask_Glow^s',
    'Circle8_00^d': 'Circle8_00^s',
    'Nt_PauseIconCh_02^d': 'Nt_PauseIconCh_02^s',            # shrine glyph
    'Nt_FadeIconKokufuku_00^d': 'DungeonClearSeal^t',        # spirit orb -> Light of Blessing (the seal glyph)
    'Nt_TempMeterNew_02^s': 'TempMeterFull^s',               # temperature gauge face
    'Nt_PauseIconL_00^s': 'IconWeapon_72x72^s',              # empty gear icons (spec section 3)
    'Nt_PauseIconL_01^s': 'IconBow_72x72^s',
    'Nt_PauseIconL_02^s': 'IconShield_72x72^s',
    'Nt_PauseIconL_05^s': 'IconCuisine_72x72^s',             # food page bowl -> TotK's cooking pot
    'Nt_HeartNewS_00^d': 'HeartW_00^s',                      # heart (spec section 3)
    'Logo2D_00^d': 'Logo_EN_L_Full_00^w',                    # game logo
    'Logo3DGameTitle_00^l': 'Logo_EN_L_Full_00^w',
}
LOGO_ASPECT = 512 / 475   # Logo_EN_L_Full_00^w
# Breath of the Wild StockItem icons -> Tears of the Kingdom item icons
ICON = {'romfs:/UI/StockItem/Obj_DRStone_Get.sbitemico#Obj_DRStone_Get': f'{M}icon/Obj_DRStone_Get'}

# Colours: Breath of the Wild's Sheikah blue family -> Tears of the Kingdom's teal family (spec C04)
TEAL = '#FF00FFC3'
COLOURS = {
    '#FF0090FD': TEAL,          # pager edges
    '#FFC8F8FF': '#FFC8FFF0',   # header text
    '#C00048FF': '#C0006E55',   # header outline
    '#996E96AA': '#996EAA9E',   # empty header
}

# Identity texts (spec C35, C38, C60)
TEXTS = {
    'The Sheikah Slate is waking...': 'The Purah Pad is waking...',
    'The Sheikah Slate cannot connect': 'The Purah Pad cannot connect',
    'Sheikah Slate open on the main screen': 'Purah Pad open on the main screen',
    'Requires Breath of the Wild version 1.6.0 or 1.9.0': 'Requires Tears of the Kingdom version 1.4.3',
    'Rune': 'Ability',
    'Champions': 'Sages',
}

# Bound names that are not botw.X -> totk.X
RENAME = {
    'botw.ready.spirit_orbs': 'totk.ready.lights',
    'botw.ready.spirit_orbs.valid': 'totk.ready.lights.valid',
    'botw.ready.clock.text': 'totk.ready.header.text',          # owner: the header switch (layer now, clock later)
    'botw.ready.clock.valid': 'totk.ready.header.valid',
    'botw.ready.recent.weapon': 'totk.ready.fuse',              # owner C19: Fuse in the secondary well
    'botw.ready.recent.weapon.valid': 'totk.ready.fuse.valid',
    'botw.ready.recent.weapon.stat': 'totk.ready.fuse.life',
    'botw.ready.effect.0.kind': 'totk.ready.effect.0.kind',
    'botw.state.menu_open': 'ui_menu_open',                     # gate on the common-state word (-1 unknown)
    'botw.ready.right.heal': 'ui_right_heal',
    'botw.ready.right.buffs': 'ui_right_buffs',
    'botw.ready.right.page': 'ui_right_page',
    'botw.card.active': '@flag:card',
    'module_error': 'ui_wrong_build',
    'botw.state.dim': 'totk.state.dim',
    'botw.state.title': 'totk.state.title',
    'botw.state.loading': 'totk.state.loading',
    'botw.state.unknown': 'totk.state.unknown',
    'botw.map.tile.1.{}': 'totk.mini.tile.{}',
}
# Names folded into expressions as constants (and gates on them resolved at build time)
CONST = {
    'botw.quick.gear': 0, 'botw.quick.touch_rune': 0, 'botw.quick.held_rune': 0,
    'botw.ready.left.quest': 0, 'botw.ready.left.champions': 1,
    'botw.map.view.cone.valid': 0,
}

# What the module publishes on the Ready, main-menu and Loading pages (0100F2C0115B6000.cpp), as patterns
PUBLISHED = [
    r'totk\.state\.(page|title|loading|unsupported|dim|menu_open|life|max_life)', r'totk\.fx\.spin\.\d+',
    r'totk\.map\.(x|y|minimap\.valid|view\.heading|view\.rotation)', r'totk\.mini\.tile\.\d+',
    r'totk\.ready\.(shield|bow|weapon)(\.valid|\.durability_pct|\.low)?', r'totk\.ready\.rune(\.valid)?',
    r'totk\.ready\.arrow(\.valid|\.count|\.count\.valid)?', r'totk\.ready\.fuse(\.valid|\.life)?',
    r'totk\.ready\.rupees\.(text|valid)', r'totk\.ready\.lights(\.valid)?', r'totk\.ready\.shrine_count(\.valid)?',
    r'totk\.ready\.sensor\.(art\.0|ring_palette\.0|text)', r'totk\.ready\.header\.(text|valid)',
    r'totk\.ready\.stamina\.(percent|depleted)', r'totk\.ready\.effects\.count',
    r'totk\.ready\.effect\.0\.(kind|icon|time)',
    r'totk\.ready\.champions\.count', r'totk\.ready\.champion_slot\.\d\.(icon|tint|display|display_kind)',
    r'totk\.ready\.heal\.(available|need_text|need_amount_text|empty)',
    r'totk\.ready\.heal\.row\.\d\.(valid|actor|name|count|full|heal_text|has_effect|effect_text|bonus_text)',
    r'totk\.quick\.(open|title|count|empty)', r'totk\.quick\.sel\.(has|name|facts)',
    r'totk\.quick\.(\d+|{i})\.(on|icon|equipped|selected|durability_pct|has_life|low|fused|fuse_icon)',
    r'totk\.(inv|quick)\.act\.(count|\d\.(on|enabled|disabled|label|reason|reason_text))',
    r'totk\.act\.(state|seq|pending|done|refused|failed|reason|line|ready|ready_reason)',
]
# Module actions the pages may name (the module's on_action)
MODULE_ACTIONS = {'totk.act', 'totk.select'}
# Bound by kept widgets, not read yet: these widgets show Breath of the Wild's empty state ('—', '—°', ...)
UNREAD = {
    'totk.ready.environment.valid': 'temperature (BR dial shows "—°")',
    'totk.ready.temperature.text': 'temperature',
    'totk.ready.area.valid': 'area name (the caption shows "Area unavailable")',
    'totk.state.shrine.active': 'in-shrine state (the disc never gives way to the shrine panel)',
    'totk.state.unknown': 'Breath of the Wild\'s "unknown context" (TotK\'s reader reports loading instead)',
}
HOST = {'module_error', 'module_error_message', 'build_match'}


def is_published(name):
    return any(re.fullmatch(p, name) for p in PUBLISHED)


def rename(name):
    """One bound name, Breath of the Wild's -> Tears of the Kingdom's (a '!' gate keeps its '!')."""
    neg = name.startswith('!')
    n = name[1:] if neg else name
    m = re.fullmatch(r'botw\.map\.tile\.1\.(\d+)', n)
    if m:
        out = f'totk.mini.tile.{m.group(1)}'
    elif n in RENAME:
        out = RENAME[n]
    elif n.startswith('botw.'):
        out = 'totk.' + n[5:]
    else:
        out = n
    return ('!' if neg else '') + out


TOKEN = re.compile(r"@[A-Za-z0-9_.:]*|[A-Za-z_][A-Za-z0-9_.]*")


def rename_expr(expr):
    def one(m):
        t = m.group(0)
        if t in CONST:
            return str(CONST[t])
        if t == 'botw.state.menu_open':
            return '(totk.state.menu_open == 1)'
        return rename(t)
    return TOKEN.sub(one, expr)


BIND_KEYS = ('bind', 'bind_text', 'text_bind', 'src_bind', 'need_bind', 'hide_bind', 'x_bind', 'y_bind',
             'rotate_bind', 'scale_bind', 'tint_bind', 'fill_bind', 'repeat_bind', 'marker_x_bind', 'marker_y_bind',
             'marker_rotate_bind', 'view_rotate_bind', 'area_bind')


def rewrite_src(src):
    if not isinstance(src, str):
        return src
    if src.startswith(R):
        t = src[len(R):]
        if t not in TEX:
            raise SystemExit(f'no Tears of the Kingdom texture for {t}')
        return lyt(TEX[t])
    if src in ICON:
        return ICON[src]
    return src


def fold_const_gates(w):
    """need_bind / hide_bind on a CONST name: decided now. Returns False when the widget can never show."""
    nb = w.get('need_bind')
    if nb and nb.lstrip('!') in CONST:
        v = CONST[nb.lstrip('!')] != 0
        if nb.startswith('!'):
            v = not v
        if not v:
            return False
        del w['need_bind']
    return True


def rewrite_widget(w):
    w = copy.deepcopy(w)
    if not fold_const_gates(w):
        return None
    # fx-driven tints (Breath of the Wild's native animation curves) keep their static tint
    if str(w.get('tint_bind', '')).startswith('botw.fx.'):
        w.pop('tint_bind')
        w.pop('tint_colors', None)
    for k in BIND_KEYS:
        if k in w and isinstance(w[k], str) and w[k]:
            w[k] = rename(w[k])
    if 'anim' in w and 'bind' in w['anim']:
        w['anim']['bind'] = rename(w['anim']['bind'])
    for k in ('src', 'marker_src', 'empty_src'):
        if k in w:
            w[k] = rewrite_src(w[k])
    if 'src_names' in w:
        w['src_names'] = [rewrite_src(s) for s in w['src_names']]
    for k in ('color', 'bg', 'tint', 'outline'):
        if isinstance(w.get(k), str) and w[k].upper() in COLOURS:
            w[k] = COLOURS[w[k].upper()]
    if w.get('text') in TEXTS:
        w['text'] = TEXTS[w['text']]
    # a text_map of Breath of the Wild msbt hashes: Tears of the Kingdom's module publishes the text itself
    if 'text_bind' in w and 'text_map' in w:
        w['bind_text'] = w.pop('text_bind')
        w.pop('text_map')
    return w


def fit_logo(rect):
    x, y, w, h = rect
    nw = round(h * LOGO_ASPECT)
    if nw <= w:
        return [round(x + (w - nw) / 2), y, nw, h]
    nh = round(w / LOGO_ASPECT)
    return [x, round(y + (h - nh) / 2), w, nh]


# ---------------------------------------------------------------------------------------------------------------

def ready_page(botw, keep_log):
    src = next(p for p in botw['pages'] if p['id'] == 'ready')['widgets']
    out = []
    by_index = {}
    for i, w0 in enumerate(src):
        if i not in KEEP_RANGES or i in DROP:
            continue
        w = rewrite_widget(w0)
        if w is None:
            keep_log.append((i, 'gate folded closed'))
            continue
        w = special(i, w)
        if w is None:
            continue
        by_index[i] = w
        out.extend(w if isinstance(w, list) else [w])
        if i == 863:   # after the four-row Champions layout: the five-row Sages layout
            out.extend(five_sage_rows(src))
    # the gear corners open the quick picker (Breath of the Wild's corner taps), while nothing else is up
    for value, r in QUICK:
        out.append({'type': 'rect', 'rect': r, 'color': '#00000000', 'frame': 0, 'on_tap': f'quick_open_{value}',
                    'need_bind': 'pp_ready_base'})
    out.extend(quick_picker())
    return {'id': 'ready', 'widgets': out}


def special(i, w):
    """Per-widget rewrites beyond the table."""
    if i == 5:   # the minimap: Tears of the Kingdom's tiles on Link's layer, Link centred, north up
        for k in ('groups', 'label_style'):
            w.pop(k, None)
        w['area'] = 'TotkMini'
        w['id'] = 'totk_minimap'
        for o in w['overlays']:
            o['src_bind'] = rename(o['src_bind'])
        return w
    if i == 68:   # the buff row's icon: the module names the picture
        w.pop('bind')
        w.pop('src_names')
        w['src_bind'] = 'totk.ready.effect.0.icon'
        w['src_hold'] = False
        return w
    if i == 96:   # the fused material: TotK's material icons carry more padding than BotW's weapon icons, so the
        # icon is cropped and enlarged to fill the well as BotW's does (its fuse-life number stays below it)
        w['rect'] = [1094, 720, 76, 76]
        w['src_rect'] = [0.12, 0.12, 0.88, 0.88]
        w['src_hold'] = False
        return w
    if i == 195:   # the condition arc (owner C19: Fuse in BotW's frame and arc). The fused material's maximum fuse
        # durability is not read (its actor pack has no LifeParameters), so the arc shows BotW's empty track only:
        # no fill and no red until a maximum exists
        for k in ('bind', 'src_names', 'hide_bind', 'keep_min', 'keep_max'):
            w.pop(k, None)
        w['src'] = 'file:assets/ready-native-secondary-condition-000.png'
        w['need_bind'] = 'totk.ready.fuse.valid'
        return w
    if i == 97:   # the empty Fuse well: the Fuse ability's mark, dimmed as Breath of the Wild's empty icons
        w['src'] = f'{M}icon/Obj_OneTouchBond'
        w['rect'] = [1100, 734, 64, 64]
        return w
    if i == 182:   # Lights of Blessing: the seal glyph fills its 56 px square
        w.pop('src_rect', None)
        return w
    if i == 196:   # the Fuse well's number: the fused material's durability points
        w['need_bind'] = 'totk.ready.fuse.valid'
        return w
    if i in (225, 243, 251, 259, 267):   # hearts: TotK's HeartW is a full-bleed heart
        w.pop('src_rect', None)
        w['rect'][3] = round(w['rect'][2] * 27 / 30)
        return w
    if i == 170:   # right pager swipes: Buffs <-> Food
        w['on_swipe_up'] = w['on_swipe_down'] = 'right_page'
        w['need_bind'] = 'ui_right_dots'
        return w
    if i in (203, 204, 208, 209):   # right pager dots: two pages while Food is offered
        w['need_bind'] = 'ui_right_dots'
        return w
    if i == 226:
        w['on_tap'] = 'card_food'
        return w
    if i == 294:
        w['on_tap'] = 'card_close'
        w.pop('payload', None)
        return w
    if i in (212, 213, 214, 215, 216, 217):   # Inventory and Map, centred as two of the three
        w['rect'][0] += 127   # 252 / 506 -> 379 / 633: the pair centred on 620
        return w
    if i in (300, 577):
        w['rect'] = fit_logo(w['rect'])
        return w
    if i == 579:   # the reason line: the host's message, or the module's when it read another build
        w2 = copy.deepcopy(w)
        w['need_bind'] = 'module_error'
        w2.update({'bind_text': 'totk.status_text', 'need_bind': 'totk.state.unsupported', 'id': 'rl_state_wrongpatch_reason_totk'})
        return [w, w2]
    if 808 <= i <= 863 and w.get('id', '').endswith('glow_ready'):   # one glow colour (BotW tints per champion)
        w['tint'] = SAGE_GLOW
        return w
    if 808 <= i <= 863 and 'icon' in w.get('id', ''):
        w['tint_colors'] = SAGE_TINTS
        w['src_hold'] = False
        return w
    return w


SAGE_TINTS = ['#FFFFFFFF', '#80FFFFFF']   # with Link / not with Link
SAGE_GLOW = '#B4B3FFFB'


def five_sage_rows(src):
    """Breath of the Wild lays out one to four Champions; Tears of the Kingdom has five sages. The fifth layout repeats
    the four-row layout's row (glow, charge glow, icon, text, separator) at an 82 px pitch, which fits five rows
    under the title inside the 468 px pane."""
    by_id = {w.get('id'): w for w in src if w.get('id', '').startswith('champion26_4_0_')}
    pitch, icon_y0 = 82, 284
    out = []
    for k in range(5):
        dy = icon_y0 + k * pitch - by_id['champion26_4_0_icon']['rect'][1]
        for part in ('glow_ready', 'glow_charge', 'icon', 'count', 'state', 'separator'):
            if part == 'separator' and k == 4:
                continue
            w = rewrite_widget(by_id[f'champion26_4_0_{part}'])
            w['id'] = f'champion26_5_{k}_{part}'
            w['rect'][1] += dy if part != 'separator' else dy - 10   # mid-gap of the shorter pitch
            name = {'glow_ready': 'glow_ready', 'glow_charge': 'glow_charge', 'icon': 'row', 'count': 'count',
                    'state': 'state', 'separator': 'row'}[part]
            w['need_bind'] = f'sage5_{k}_{name}'
            for key in ('src_bind', 'bind_text', 'tint_bind'):
                if key in w:
                    w[key] = w[key].replace('champion_slot.0.', f'champion_slot.{k}.')
            if part == 'glow_ready':
                w['tint'] = SAGE_GLOW
            if part == 'icon':
                w['tint_colors'] = SAGE_TINTS
                w['src_hold'] = False
            out.append(w)
    return out


def five_sage_derived():
    d = []
    for k in range(5):
        base = 'pp_ready_base == 1 && totk.ready.champions.count == 5'
        kind = f'totk.ready.champion_slot.{k}.display_kind'
        d += [{'name': f'sage5_{k}_row', 'expr': base},
              {'name': f'sage5_{k}_glow_ready', 'expr': f'{base} && {kind} == 1'},
              {'name': f'sage5_{k}_glow_charge', 'expr': f'{base} && {kind} == 2'},
              {'name': f'sage5_{k}_count', 'expr': f'{base} && {kind} == 1'},
              {'name': f'sage5_{k}_state', 'expr': f'{base} && {kind} != 1'}]
    return d


def simple_page(botw, pid, drop):
    src = next(p for p in botw['pages'] if p['id'] == pid)['widgets']
    out = []
    for i, w0 in enumerate(src):
        if i in drop:
            continue
        w = rewrite_widget(w0)
        if w is None:
            continue
        if w.get('src', '').startswith(lyt('Logo_')):
            w['rect'] = fit_logo(w['rect'])
        out.append(w)
    return {'id': pid, 'widgets': out}


# ---------------------------------------------------------------------------------------------------------------
# Inventory and Map: Tears of the Kingdom's own pages, in Breath of the Wild's palette and type
# ---------------------------------------------------------------------------------------------------------------

INK, MUTED, WHITE = '#FFFFFCC6', '#B4FFFCC6', '#FFFFFFFF'
PAGE, PANE, LINE = '#FF0C0F11', '#7D000000', '#46FFFCC6'
RED = '#FFEC4A34'
ITALIC = {'italic': True, 'slant_deg': 9.0, 'text_role': 'menu_text'}


def label(x, y, text=None, scale=5, color=INK, align='center', width=0, bind=None, **kw):
    """A Breath of the Wild-style label: rect x is the anchor (left edge, centre or right edge by align)."""
    out = {'type': 'label', 'rect': [round(x), round(y), round(width), 0], 'text_scale': scale, 'color': color,
           'align': align, **ITALIC}
    out['text'] = text if text is not None else ''
    if bind:
        out['bind_text'] = bind
    if width:
        out.update({'wrap_width': round(width), 'max_lines': 1, 'fit_text': True, 'text_min_scale': max(3, scale - 1)})
    out.update(kw)
    return out


def rect(x0, y0, x1, y1, fill, line=None, frame=2, **kw):
    out = {'type': 'rect', 'rect': [round(x0), round(y0), round(x1 - x0), round(y1 - y0)], 'bg': fill,
           'color': line or '#00000000', 'frame': frame if line else 0}
    out.update(kw)
    return out


def image(x0, y0, x1, y1, src=None, bind=None, **kw):
    out = {'type': 'image', 'rect': [round(x0), round(y0), round(x1 - x0), round(y1 - y0)]}
    if src:
        out['src'] = src
    if bind:
        out['src_bind'] = bind
    out.update(kw)
    return out


def corner_card(x0, y0, x1, y1, **kw):
    """Breath of the Wild's card: the corner mask and frame, stretched."""
    sl = {'fill': 'slice', 'slice': [8, 8, 8, 8]}   # the art's corners are 6 px: stretch the edges only
    return [image(x0, y0, x1, y1, 'file:assets/ready-corner-mask.png', tint='#7D000000', **sl, **kw),
            image(x0, y0, x1, y1, 'file:assets/ready-corner-frame.png', tint=LINE, **sl, **kw)]


def command_button(x, y, w, text, action, **kw):
    return [image(x, y, x + w, y + 112, 'file:assets/cmdbar-228x112-normal.png', tint='#FFFFFFFF', **kw),
            label(x + w / 2, y + 35, text, 6, INK, width=w - 28, **kw),
            rect(x, y, x + w, y + 112, '#00000000', on_tap=action, **kw)]


def tiles(prefix):
    """The 12x10 MainField tiles as map overlays: tile (c, r) covers x -6000+1000c.., z -5000+1000r..
    (row 0 is north); map y is north-up (-z)."""
    return [{'src_bind': f'{prefix}.{r * 12 + c}', 'x0': -6000 + 1000 * c, 'x1': -5000 + 1000 * c,
             'y0': 4000 - 1000 * r, 'y1': 5000 - 1000 * r} for r in range(10) for c in range(12)]


TABS = [('Weapons', 'IconWeapon_72x72^s'), ('Bows', 'IconBow_72x72^s'), ('Shields', 'IconShield_72x72^s'),
        ('Armor', 'IconEquipment_72x72^s'), ('Materials', 'IconMaterial_72x72^s'), ('Food', 'IconCuisine_72x72^s'),
        ('Zonai', 'IconRecycle_72x72^s'), ('Key items', 'Nt_PauseIconL_06^s')]
MAX_ROWS = 300
CELL_W, CELL_H, COLS = 196, 150, 6


# Breath of the Wild's Inventory and Map pages end with the same overlay block as its Ready page: the title,
# loading and unknown-state covers, the wrong-build cover, the event dim and the pause-menu band (spec C33, C34,
# C57-C60, C74). Ours are the Ready page's widgets, through the same rewrite table.
OVERLAYS = span(295, 319) | span(573, 581) | span(587, 592)


def overlay_block(botw):
    src = next(p for p in botw['pages'] if p['id'] == 'ready')['widgets']
    out = []
    for i in sorted(OVERLAYS - DROP):
        w = rewrite_widget(src[i])
        if w is None:
            continue
        w = special(i, w)
        out.extend(w if isinstance(w, list) else [w])
    return out


def inventory_page(botw):
    w = [rect(0, 0, 1240, 1080, PAGE)]
    for i, (name, icon) in enumerate(TABS):
        x = 16 + i * 134
        w += corner_card(x, 16, x + 126, 96, )
        w.append(rect(x, 16, x + 126, 96, '#00000000', on_tap=f'inv_tab_{i}'))
        w.append(rect(x + 8, 88, x + 118, 92, TEAL, need_bind=f'totk.inv.tab.{i}'))
        w.append(image(x + 35, 24, x + 91, 80, lyt(icon), tint='#809DAEAB', hide_bind=f'totk.inv.tab.{i}', keep_min=0,
                       keep_max=0))
        w.append(image(x + 35, 24, x + 91, 80, lyt(icon), tint=INK, need_bind=f'totk.inv.tab.{i}'))
    w += command_button(1096, 4, 136, 'Back', 'page_ready')
    grid = {'scroll': 'inv_grid', 'repeat': MAX_ROWS, 'repeat_cols': COLS, 'repeat_dx': CELL_W + 4,
            'repeat_row_dy': CELL_H}
    x0, y0 = 16, 128
    w.append({'type': 'image', 'src': 'file:assets/ready-corner-mask.png', 'rect': [x0, y0, CELL_W, CELL_H - 8],
              'tint': '#7D000000', 'need_bind': 'totk.inv.{i}.on', **grid})
    w.append({'type': 'image', 'src': 'file:assets/ready-corner-frame.png', 'rect': [x0, y0, CELL_W, CELL_H - 8],
              'tint': LINE, 'need_bind': 'totk.inv.{i}.on', 'on_tap': 'inv_sel_{i}', **grid})
    w.append({'type': 'image', 'src': 'file:assets/ready-corner-frame.png', 'rect': [x0, y0, CELL_W, CELL_H - 8],
              'tint': TEAL, 'need_bind': 'totk.inv.{i}.equipped', **grid})
    w.append({'type': 'rect', 'rect': [x0, y0, CELL_W, CELL_H - 8], 'bg': '#3000FFC3', 'color': '#FFFFFEE4',
              'frame': 3, 'need_bind': 'totk.inv.{i}.selected', **grid})
    w.append({'type': 'image', 'rect': [x0 + 50, y0 + 8, 96, 96], 'src_bind': 'totk.inv.{i}.icon',
              'need_bind': 'totk.inv.{i}.on', 'src_hold': False, **grid})
    w.append({'type': 'image', 'rect': [x0 + 8, y0 + 8, 40, 40], 'src_bind': 'totk.inv.{i}.fuse_icon',
              'need_bind': 'totk.inv.{i}.fused', 'src_hold': False, **grid})
    w.append({'type': 'label', 'rect': [x0 + CELL_W - 12, y0 + 100, 0, 0], 'bind_text': 'totk.inv.{i}.qty',
              'text_scale': 4, 'color': WHITE, 'align': 'right', 'outline': '#C8000A19', 'outline_px': 2,
              'italic': False, 'slant_deg': 0.0, 'text_role': 'numeral_readout', 'need_bind': 'totk.inv.{i}.on', **grid})
    w.append({'type': 'bar', 'rect': [x0 + 16, y0 + 124, CELL_W - 32, 8], 'bind': 'totk.inv.{i}.life',
              'max_bind': 'totk.inv.{i}.life_max', 'color': INK, 'bg': '#40FFFCC6', 'frame': 0,
              'need_bind': 'totk.inv.{i}.has_life', **grid})
    w.append(label(620, 470, 'No items in this category', 5, need_bind='totk.inv.empty'))
    # the selected item: its name, facts and caption on the left, its actions on the right (Breath of the Wild's
    # Inventory action bar: the primary action rightmost), the latest request's line above them
    w += corner_card(16, 868, 1224, 1064)
    sel = {'need_bind': 'totk.inv.sel.has'}
    w.append(label(40, 884, None, 6, INK, 'left', 500, 'totk.inv.sel.name', **sel))
    w.append(label(40, 930, None, 4, '#FFF6DC9D', 'left', 500, 'totk.inv.sel.facts', **sel))
    w.append({'type': 'label', 'rect': [40, 968, 500, 0], 'bind_text': 'totk.inv.sel.caption', 'text_scale': 4,
              'color': MUTED, 'wrap_width': 500, 'max_lines': 3, 'align': 'left', **ITALIC, **sel})
    w.append(label(620, 950, 'Tap an item to see it here', 5, MUTED, need_bind='totk.inv.sel.none'))
    for k in range(3):
        w += action_button(1004 - k * 224, 944, 216, f'totk.inv.act.{k}', f'totk_inv_act_{k}')
    w += result_line(556, 892, 664)
    w += overlay_block(botw)
    scroll = {'id': 'inv_grid', 'rect': [x0, y0, 1208, 732], 'count_bind': 'totk.inv.count', 'row_h': CELL_H,
              'cols': COLS, 'reset_bind': '@flag:inv_tab', 'bar': '#80FFFCC6', 'bar_track': '#20FFFCC6'}
    return {'id': 'inventory', 'widgets': w, 'scrolls': [scroll]}


def action_button(x, y, width, p, action):
    """One item action (the module's <p>.on / .enabled / .disabled / .label / .reason_text): Breath of the Wild's
    command bar with its verb; a disabled one dimmed, with the reason under the verb. A tap on either asks the module
    (a disabled one is refused: the refused haptic, and the line says why)."""
    art = f'file:assets/cmdbar-{width}x112-normal.png'
    cx = x + width / 2
    return [
        image(x, y, x + width, y + 112, art, tint='#FFFFFFFF', need_bind=f'{p}.enabled'),
        image(x, y, x + width, y + 112, art, tint='#59FFFFFF', need_bind=f'{p}.disabled'),
        label(cx, y + 35, None, 6, INK, width=width - 28, bind=f'{p}.label', need_bind=f'{p}.enabled'),
        label(cx, y + 22, None, 5, MUTED, width=width - 28, bind=f'{p}.label', need_bind=f'{p}.disabled'),
        {'type': 'label', 'rect': [round(cx), y + 62, width - 24, 0], 'bind_text': f'{p}.reason_text', 'text_scale': 3,
         'color': MUTED, 'align': 'center', 'wrap_width': width - 24, 'max_lines': 2, 'fit_text': True,
         'text_min_scale': 3, **ITALIC, 'need_bind': f'{p}.disabled'},
        rect(x, y, x + width, y + 112, '#00000000', on_tap=action, need_bind=f'{p}.on'),
    ]


def result_line(x, y, width):
    """The latest request: pending in the page's ink, done in teal, refused and failed in red."""
    out = []
    for flag, colour in (('pending', INK), ('done', TEAL), ('refused', RED), ('failed', RED)):
        out.append(label(x, y, None, 4, colour, 'left', width, 'totk.act.line', need_bind=f'totk.act.{flag}'))
    return out


# The quick picker (Breath of the Wild's, for its three gear corners): a tap on the weapon, bow or shield corner opens
# that pouch over the Ready page; the equipped item is selected, a tap selects another, the confirm bar equips or
# unequips it. QUICK: (flag value, the corner's tap area) as Breath of the Wild's corner taps (widgets 133, 116, 107).
QUICK = [(1, [1040, 872, 184, 192]), (2, [1040, 16, 184, 192]), (3, [16, 16, 184, 192])]
QUICK_CELLS, QUICK_COLS, QUICK_PITCH, QUICK_CELL = 21, 7, 150, 140


def quick_picker():
    gate = {'need_bind': 'ui_quick'}
    x0, y0 = 620 - (QUICK_COLS * QUICK_PITCH - (QUICK_PITCH - QUICK_CELL)) // 2, 270
    cell = {'repeat': QUICK_CELLS, 'repeat_bind': 'totk.quick.count', 'repeat_cols': QUICK_COLS,
            'repeat_dx': QUICK_PITCH, 'repeat_row_dy': QUICK_PITCH}
    c = QUICK_CELL
    w = [rect(0, 0, 1240, 1080, '#A5000000', input_block=True, **gate),
         rect(64, 72, 1176, 1012, '#FF101416', '#46FFFCC6', 1, **gate),
         rect(64, 72, 70, 1012, TEAL, **gate),
         label(1152, 96, None, 5, INK, 'right', bind='totk.quick.title', **gate),
         label(1152, 136, 'Tap an item to inspect', 5, '#A8FFFCC6', 'right', **gate),
         label(90, 228, 'Pouch', 5, '#A8FFFCC6', 'left', **gate),
         {'type': 'value', 'rect': [1152, 228, 0, 0], 'bind': 'totk.quick.count', 'text_scale': 5, 'color': WHITE,
          'align': 'right', 'suffix': ' in the pouch', 'italic': False, 'slant_deg': 0.0,
          'text_role': 'numeral_readout', **gate},
         label(620, 420, 'Nothing in this pouch', 5, MUTED, need_bind='totk.quick.empty')]
    w.append({'type': 'rect', 'rect': [x0, y0, c, c], 'bg': '#C8000000', 'color': '#3CFFFCC6', 'frame': 1,
              'need_bind': 'totk.quick.{i}.on', **cell})
    w.append({'type': 'rect', 'rect': [x0 + 2, y0 + 2, c - 4, c - 4], 'bg': '#B400FFC3', 'color': '#00000000',
              'frame': 0, 'need_bind': 'totk.quick.{i}.equipped', **cell})
    w.append({'type': 'image', 'rect': [x0 + 15, y0 + 12, c - 30, c - 30], 'src_bind': 'totk.quick.{i}.icon',
              'need_bind': 'totk.quick.{i}.on', 'src_hold': False, **cell})
    w.append({'type': 'image', 'rect': [x0 + 6, y0 + 6, 40, 40], 'src_bind': 'totk.quick.{i}.fuse_icon',
              'need_bind': 'totk.quick.{i}.fused', 'src_hold': False, **cell})
    w.append({'type': 'bar', 'rect': [x0 + 14, y0 + c - 18, c - 28, 8], 'bind': 'totk.quick.{i}.durability_pct',
              'max': 100, 'bg': '#40FFFCC6', 'color': INK, 'frame': 0, 'need_bind': 'totk.quick.{i}.has_life', **cell})
    w.append({'type': 'bar', 'rect': [x0 + 14, y0 + c - 18, c - 28, 8], 'bind': 'totk.quick.{i}.durability_pct',
              'max': 100, 'bg': '#40FFFCC6', 'color': RED, 'frame': 0, 'need_bind': 'totk.quick.{i}.low', **cell})
    w.append({'type': 'rect', 'rect': [x0 + 2, y0 + 2, c - 4, c - 4], 'bg': '#30FFFCC6', 'color': '#00000000',
              'frame': 0, 'need_bind': 'totk.quick.{i}.selected', **cell})
    w.append({'type': 'rect', 'rect': [x0 + 1, y0 + 1, c - 2, c - 2], 'bg': '#00000000', 'color': TEAL, 'frame': 2,
              'pulse': True, 'need_bind': 'totk.quick.{i}.selected', **cell})
    w.append({'type': 'rect', 'rect': [x0, y0, c, c], 'color': '#00000000', 'frame': 0, 'on_tap': 'quick_sel_{i}',
              'need_bind': 'totk.quick.{i}.on', **cell})
    sel = {'need_bind': 'totk.quick.sel.has'}
    w.append(label(90, 730, None, 6, INK, 'left', 1062, 'totk.quick.sel.name', **sel))
    w.append(label(90, 780, None, 4, '#FFF6DC9D', 'left', 1062, 'totk.quick.sel.facts', **sel))
    w += result_line(90, 826, 1062)
    # Breath of the Wild's confirm bar: Back on the left, the item's action on the right
    w.append(image(88, 880, 608, 992, 'file:assets/cmdbar-520x112-normal.png', tint='#FFFFFFFF', **gate))
    w.append(label(348, 915, 'Back', 6, INK, width=492, **gate))
    w.append(rect(88, 880, 608, 992, '#00000000', on_tap='quick_close', **gate))
    w += action_button(632, 880, 520, 'totk.quick.act.0', 'totk_quick_act_0')
    return w


def map_page(botw):
    w = [rect(0, 0, 1240, 1080, '#FF000D17'),
         {'type': 'map', 'id': 'totk_map', 'area': 'TotkFull', 'rect': [0, 0, 1240, 1080], 'area_label': False,
          'pan_zoom': True, 'min_zoom': 1, 'max_zoom': 12, 'view_idle_ms': 0,
          'marker_x_bind': 'totk.map.x', 'marker_y_bind': 'totk.map.y', 'marker_src': lyt('Nt_MapPlayerIconNew_00^s'),
          # Breath of the Wild's full-map marker (its hyrule_map widget): 101 px, yellow
          'marker_size': [101, 101], 'marker_tint': '#FFF5F506', 'marker_rotate_bind': 'totk.map.heading',
          'overlays': tiles('totk.map.tile')}]
    w.append(label(620, 1032, 'Map unavailable in this area', 6, need_bind='!totk.map.valid'))
    buttons = [('Back', 'page_ready', None), ('Surface', 'map_layer_1', 'totk.map.page_layer_is.0'),
               ('Sky', 'map_layer_2', 'totk.map.page_layer_is.1'), ('Depths', 'map_layer_3', 'totk.map.page_layer_is.2'),
               ('Follow Link', 'map_layer_0', None), ('Recenter', 'map_recenter', None)]
    for i, (text, action, lit) in enumerate(buttons):
        y = 8 + i * 120 + (24 if i else 0)
        w += command_button(8, y, 228, text, action)
        if lit:
            w.append(rect(40, y + 92, 204, y + 96, TEAL, need_bind=lit))
    # where Link is (spec C48): whole metres, signed, east / height / south
    w.append(rect(380, 1016, 860, 1072, '#B4000000'))
    w.append({'type': 'label', 'rect': [620, 1032, 460, 0], 'bind_text': 'totk.coords', 'text_scale': 4,
              'color': WHITE, 'align': 'center', 'italic': False, 'slant_deg': 0.0, 'text_role': 'numeral_readout',
              'need_bind': 'totk.map.valid'})
    w += overlay_block(botw)
    return {'id': 'map', 'widgets': w}


# ---------------------------------------------------------------------------------------------------------------

def collect(o, f):
    if isinstance(o, dict):
        for k, v in o.items():
            f(k, v)
            collect(v, f)
    elif isinstance(o, list):
        for v in o:
            collect(v, f)


def derived_entries(botw, pages):
    """The derived values the kept widgets need (renamed from Breath of the Wild's), plus Tears of the Kingdom's."""
    d = {x['name']: x for x in botw['derived']}
    ours = [
        {'name': 'ui_menu_open', 'expr': 'totk.state.menu_open == 1'},
        {'name': 'ui_wrong_build', 'expr': 'module_error == 1 || totk.state.unsupported == 1'},
        # right pager: Buffs (0) and, while the hearts are low, Food (1)
        {'name': 'ui_right_page', 'expr': '(totk.ready.heal.available == 1 && @flag:right_pane == 1) ? 1 : 0'},
        {'name': 'ui_right_buffs', 'expr': 'ui_right_page == 0'},
        {'name': 'ui_right_heal', 'expr': 'ui_right_page == 1'},
        {'name': 'ui_right_dots', 'expr': 'pp_ready_base == 1 && totk.ready.heal.available == 1'},
        # the food card closes itself when the hearts are no longer low
        {'name': 'rcf_food_on', 'expr': '(@flag:card == 3 && totk.ready.heal.available == 1) ? 1 : 0'},
        {'name': 'rcf_any', 'expr': 'rcf_food_on'},
        {'name': 'states_cover', 'terms': [['totk.state.dim', 1]]},
        # the quick picker: open while the module shows it (it gives way to the game's menu, events and loading)
        {'name': 'ui_quick', 'expr': 'totk.quick.open == 1 && totk.state.page == 0'},
    ] + five_sage_derived()
    defined = {x['name'] for x in ours}
    need = set()

    def want(k, v):
        if isinstance(v, str) and (k.endswith('_bind') or k in ('bind', 'bind_text')):
            need.add(v.lstrip('!'))
    for p in pages:
        collect(p['widgets'], want)
    for x in ours:
        for t in TOKEN.findall(x.get('expr', '')):
            need.add(t)
    out, seen = [], set()

    def add(name):
        if name in seen or name in defined:
            return
        seen.add(name)
        src = next((x for x in botw['derived'] if rename_expr(x['name']) == name or x['name'] == name), None)
        if src is None:
            return
        x = copy.deepcopy(src)
        x['name'] = name
        if 'expr' in x:
            x['expr'] = rename_expr(x['expr'])
            if name == 'pp_ready_base':
                # the Sages rows are drawn after the state covers (Breath of the Wild's widget order) and the
                # module keeps its last values while the title or a loading screen shows: hide them there too
                x['expr'] += ' && totk.state.page == 0 && totk.state.loading == 0'
            for t in TOKEN.findall(x['expr']):
                add(t)
        elif 'terms' in x:
            x['terms'] = [[rename(t[0]), t[1]] for t in x['terms'] if not str(t[0]).startswith('botw.state.shrine')]
            for t in x['terms']:
                add(t[0])
        out.append(x)
    for n in sorted(need):
        add(n)
    for x in ours:
        for t in TOKEN.findall(x.get('expr', '')):
            add(t)
    return ours + out


def actions():
    page = lambda p: {'kind': 'page', 'page': p, 'transition': 'fade', 'duration_ms': 90, 'easing': 'ease_in_out'}
    a = {'inventory': page('inventory'), 'map': page('map'), 'page_ready': page('ready'),
         'right_page': {'kind': 'flag', 'flag': 'right_pane'},
         'card_food': {'kind': 'flag', 'flag': 'card', 'value': 3},
         'card_close': {'kind': 'flag', 'flag': 'card', 'value': 0},
         'map_recenter': {'kind': 'view_reset', 'view': 'totk_map'}}
    for i in range(len(TABS)):
        a[f'inv_tab_{i}'] = {'kind': 'flag', 'flag': 'inv_tab', 'value': i}
    for i in range(MAX_ROWS):
        a[f'inv_sel_{i}'] = {'kind': 'flag', 'flag': 'inv_sel', 'value': i}
    for i in range(4):
        a[f'map_layer_{i}'] = {'kind': 'flag', 'flag': 'map_layer', 'value': i}
    # item actions (the module's "totk.act": source * 8 + button; source 0 the Inventory detail, 1 the quick picker)
    for k in range(3):
        a[f'totk_inv_act_{k}'] = {'kind': 'module', 'action': 'totk.act', 'argument': k, 'haptic': 'confirm'}
    a['totk_quick_act_0'] = {'kind': 'module', 'action': 'totk.act', 'argument': 8, 'haptic': 'confirm'}
    for value, _ in QUICK:
        a[f'quick_open_{value}'] = {'kind': 'flag', 'flag': 'quick', 'value': value}
    a['quick_close'] = {'kind': 'flag', 'flag': 'quick', 'value': 0}
    for i in range(QUICK_CELLS):
        a[f'quick_sel_{i}'] = {'kind': 'module', 'action': 'totk.select', 'argument': i}
    return a


def composites(botw, pages):
    used = set()
    collect(pages, lambda k, v: used.add(v[10:]) if isinstance(v, str) and v.startswith('composite:') else None)
    out = {}
    for name in sorted(used):
        c = copy.deepcopy(botw['composites'][name])
        for layer in c['layers']:
            for k in ('src', 'mask_src'):
                if k in layer:
                    layer[k] = rewrite_src(layer[k])
        out[name] = c
    return out


def files_used(obj):
    used = set()
    collect(obj, lambda k, v: used.add(v[5:]) if isinstance(v, str) and v.startswith('file:assets/') else None)
    collect(obj, lambda k, v: [used.add(s[5:]) for s in v if isinstance(s, str) and s.startswith('file:assets/')]
            if k == 'src_names' else None)
    return used


def copy_assets(out_dir, used):
    """Copies the Breath of the Wild package's generated art the pages use, with its provenance."""
    prov = json.load(open(os.path.join(BOTW_DIR, 'ASSETS.provenance.json')))
    try:
        commit = subprocess.run(['git', '-C', ROOT, 'log', '-1', '--format=%H', '--', 'packages/BreathOfTheWild'],
                                capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        commit = 'unknown'
    dst = os.path.join(out_dir, 'assets')
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    files = {}
    for f in sorted(used):
        shutil.copy2(os.path.join(BOTW_DIR, f), os.path.join(dst, os.path.basename(f)))
        entry = next((v for g, v in prov['files'].items() if fnmatch.fnmatch(f, g)), None)
        if entry is None or entry.get('origin') != 'generated':
            raise SystemExit(f'{f}: not listed as generated art in the Breath of the Wild provenance')
        files[f] = {'origin': 'generated', 'source': entry['source'],
                    'copied_from': f'packages/BreathOfTheWild/dualscreen/{f} at {commit}'}
    with open(os.path.join(out_dir, 'ASSETS.provenance.json'), 'w') as fp:
        json.dump({'format': 1, '_note': 'Eden Duo-generated art (GPL-3.0-or-later), copied unchanged from the Breath '
                   'of the Wild package by tools/totk/gen_manifest.py. No game art is shipped.', 'files': files},
                  fp, indent=1)
        fp.write('\n')


def check(m):
    """No Breath of the Wild names, paths or module actions left; every bound totk. name accounted for."""
    bad, bound = [], set()

    def visit(k, v):
        if isinstance(v, str):
            if 'botw.' in v or v.startswith('romfs:') or 'Bootup.pack' in v:
                bad.append((k, v))
            if k.endswith('_bind') or k in ('bind', 'bind_text'):
                bound.add(v.lstrip('!'))
    collect(m['pages'], visit)
    collect(m['derived'], visit)
    collect(m['composites'], visit)
    for name, a in m['actions'].items():
        if a['kind'] not in ('page', 'flag', 'view_reset') and not (
                a['kind'] == 'module' and a.get('action') in MODULE_ACTIONS):
            bad.append(('action', name))
    if bad:
        raise SystemExit(f'left over from Breath of the Wild: {bad[:10]}')
    derived = {x['name'] for x in m['derived']}
    for x in m['derived']:
        for t in TOKEN.findall(x.get('expr', '')) + [t[0] for t in x.get('terms', [])]:
            if not t.startswith('@'):
                bound.add(t)
    page_names = set()
    for p in m['pages']:
        if p['id'] in ('inventory', 'map'):
            collect(p['widgets'], lambda k, v: page_names.add(v.lstrip('!')) if isinstance(v, str) and (
                k.endswith('_bind') or k in ('bind', 'bind_text')) else None)
    unknown = sorted(n for n in bound if n.startswith('totk.') and not is_published(n) and n not in UNREAD
                     and not n.startswith(('totk.inv.', 'totk.map.', 'totk.coords', 'totk.status_text'))
                     and n not in derived)
    if unknown:
        raise SystemExit(f'bound but neither published nor listed as unread: {unknown}')
    return sorted(n for n in bound if n in UNREAD)


def manifest(botw, keep_log):
    ready = ready_page(botw, keep_log)
    mainmenu = simple_page(botw, 'mainmenu', {0, 2, 3, 6})    # fx probe, conductor frames, pre-slate text
    loading = simple_page(botw, 'loading', {0})
    for w in loading['widgets']:
        if w.get('src', '').startswith(lyt('Logo_')):
            w['tint'] = '#FFADAC9B'
    pages = [ready, inventory_page(botw), map_page(botw), mainmenu, loading]
    world = {'min': [-6000, -5000], 'max': [6000, 5000], 'image': f'{M}gen/clear', 'no_pin': False}
    m = {
        'format': 1,
        'title_id': '0100F2C0115B6000',
        'name': 'Tears of the Kingdom companion',
        'min_runtime': 22,
        'requires_module': True,
        'module': {'abi': 1, 'build_ids': [BUILD_143]},
        'canvas_w': 1240,
        'canvas_h': 1080,
        'background': botw['background'],
        'font': 'file:totk_font.txt',
        'font_atlas': f'{M}font/{FONT}',
        '_note': f'Version {VERSION}. Generated by tools/totk/gen_manifest.py from the Breath of the Wild package\'s '
                 'Ready, main-menu and Loading pages. All game art and text are decoded at runtime from the player\'s '
                 'own romfs by the native module; the package ships none.',
        'flags': {'card': 0, 'right_pane': 0, 'inv_tab': 0, 'inv_sel': -1, 'map_layer': 0, 'quick': 0},
        'map': {'areas': {'TotkMini': dict(world, clamp_view=False), 'TotkFull': dict(world, clamp_view=True)},
                'style': {'background': '#FF000D17'}},
        'actions': actions(),
        'haptics': botw['haptics'],
        'page_binds': [{'point': 'totk.state.page', 'equals': v, 'when_equal': {
            'page': p, 'transition': 'fade', 'duration_ms': ms, 'easing': 'ease_in_out'}}
            for v, p, ms in ((1, 'mainmenu', 130), (2, 'loading', 100), (0, 'ready', 90))],
        'module_tick_hidden': True,
        'pages': pages,
    }
    m['derived'] = derived_entries(botw, pages)
    m['composites'] = composites(botw, pages)
    pre = set()
    collect([ready, mainmenu, loading, m['composites']],
            lambda k, v: pre.add(v) if isinstance(v, str) and k in ('src', 'marker_src') and (
                v.startswith(lyt('')) or v.startswith('file:')) else None)
    m['preload'] = sorted(pre)
    return m


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    out_dir = os.path.dirname(os.path.abspath(out))
    botw = json.load(open(os.path.join(BOTW_DIR, 'manifest.json')))
    keep_log = []
    m = manifest(botw, keep_log)
    unread = check(m)
    os.makedirs(out_dir, exist_ok=True)
    copy_assets(out_dir, files_used(m))
    with open(out, 'w') as f:
        json.dump(m, f, separators=(',', ':'))
        f.write('\n')
    with open(os.path.join(out_dir, 'totk_font.txt'), 'w') as f:
        f.write("# Game font for the companion pages: <font in /Font/Font.Nin_NX_NVN.bfarc.zs>:<cap height px>.\n"
                "# The module rasterises it from the player's own romfs; nothing is shipped.\n" + FONT + '\n')
    n = {p['id']: len(p['widgets']) for p in m['pages']}
    print(f'{out}: {os.path.getsize(out)} bytes; widgets {n}; derived {len(m["derived"])}; '
          f'not read yet: {", ".join(unread)}')


if __name__ == '__main__':
    main()
