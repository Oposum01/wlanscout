// SPDX-License-Identifier: CC-BY-NC-SA-4.0
// SPDX-FileCopyrightText: Dtmcnamara (the original MagSafe case)
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>
//
// xiao_c5_case.scad - MagSafe case for the Seeed Studio XIAO ESP32-C5 with
// the FPC antenna slid under tabs on the lid underside, behind the board.
//
// Derived from esp32-c5-magsafe-case-v7.scad, itself adapted from "Biscuit -
// DIY MagSafe case, ESP32-C5 Waveshare" by Dtmcnamara on MakerWorld
// (https://makerworld.com/de/models/2590419), CC BY-NC-SA 4.0 like it.
//
// Version: 1.8.0
//
// OpenSCAD port of xiao_c5_case.py v1.8.0 (same parameters and geometry).
// Coordinates: x across (viewer's right = +x), y along (USB end at -y),
// z up, MagSafe ring centre at the origin.
//
// The FPC lies flat against the lid underside between the collar legs and is
// held by tabs (lips on the collar inner faces, ANT_SLOT_H below the lid).
// It is pushed in from the front; ramp nubs stop it there, the collar end
// segment is the far end stop. Print the lid upside down (part = "lid_print"):
// the lips are short one-sided overhangs, no support needed.
//
// Values tagged UNVERIFIED are assumptions; "measured" values come from
// Jan's board. Tested with OpenSCAD 2021.01.

part = "bottom";               // [bottom, lid, lid_print, assembly]
side_profile = "concave";      // [concave, convex]

$fn = 64;

// --- sourced values --------------------------------------------------------
// XIAO ESP32-C5 board outline 21 x 17.8 mm (Seeed spec)
PCB_L = 21.0;
PCB_W = 17.8;
// FPC antenna (same as XIAO ESP32-C3, MyAntenna M01-0600890R0A datasheet):
// FPC 40 x 20 mm, cable 1.13 mm x 80 mm
ANT_L = 40.0;
ANT_W = 20.0;

// --- measured on the board ---------------------------------------------------
PCB_T = 1.27;                  // measured
TALL_H = 4.50;                 // measured: USB-C, from PCB underside
USB_BODY_H = 3.20;             // measured: USB-C above PCB top
USB_OVERHANG = 2.25;           // measured: USB-C beyond the USB-end PCB edge
USB_LEN = 7.38;                // measured: USB-C receptacle length
SHIELD_GAP = 0.3;              // measured: USB-C rear end -> RF shield
SHIELD_L = 11.5;               // measured: RF shield length (y)
SHIELD_W = 13.5;               // measured: RF shield width (x)
SHIELD_H = 2.98;               // measured: RF shield top from PCB underside
SHIELD_EDGE_L = 1.99;          // measured: left long edge -> shield (USB end facing the viewer)
SHIELD_EDGE_R = 2.30;          // measured: right long edge -> shield
USB_EDGE_L = 4.35;             // measured: PCB left edge -> USB-C left side
USB_EDGE_R = 4.49;             // measured: PCB right edge -> USB-C right side
USB_W = PCB_W - USB_EDGE_L - USB_EDGE_R;       // 8.96
USB_X = -PCB_W / 2 + USB_EDGE_L + USB_W / 2;   // -0.07
BTN_W = 3.0;                   // measured button size across x
BTN_L = 2.5;                   // measured button size along y
BTN_H = 2.50;                  // measured: button top from PCB underside
BTN_EDGE_LEFT = 3.53;           // measured: left long edge -> left button housing
BTN_EDGE_RIGHT = 3.13;          // measured: right long edge -> right button housing
BTN_X_LEFT = -PCB_W / 2 + BTN_EDGE_LEFT + BTN_W / 2;     // -3.87
BTN_X_RIGHT = PCB_W / 2 - BTN_EDGE_RIGHT - BTN_W / 2;    // +4.27
// buttons flush with the far PCB edge (stated)
FPC_T = 0.23;                  // measured: FPC incl. adhesive
FEED_W = 6.6; FEED_L = 3.3; FEED_H = 1.63;     // measured feed joint, centred
// U.FL (measured): far PCB edge, centred between the buttons; the mated plug
// overhangs the far edge -> posts stay outside, centre behind the PCB is free

// --- UNVERIFIED assumptions ------------------------------------------------
MEASURED = true;               // all board values measured
// pressure pad on the USB-C shell: range from the USB-end PCB edge, 0.5 mm
// inside the on-board part of the receptacle (7.38 - 2.25 = 5.13 mm)
USB_PAD_Y = [0.5, USB_LEN - USB_OVERHANG - 0.5];

// buttons: [marking, x, button centre from USB-end PCB edge, top height from
// PCB underside]; marking is engraved text, or CAMERA for the camera pictogram
CAMERA = "@camera";
// left / right as seen from the USB end, lid up (= -x / +x): boot (camera
// pictogram) left, reset right
BUTTONS = [[CAMERA, BTN_X_LEFT, PCB_L - BTN_L / 2, BTN_H],      // boot
           ["RESET", BTN_X_RIGHT, PCB_L - BTN_L / 2, BTN_H]];

// --- geometry parameters (mm) ---------------------------------------------
PLATE_T = 1.5;
PLATE_EDGE_R = 1.0;
PLATE_RING_MARGIN = 2.5;
PLATE_STRIP_MARGIN = 2.5;
PLATE_SHAPE = "v137";          // "v137": outline of the v1.37.0 case, "hull": generated
// base plate outline of the v1.37.0 case, from
// esp32-c5-magsafe-case-base-v7.stl (pcb_box_case.py v1.37.0 bottom), section at z = 0.3, y shifted by -100 (ring centre at the origin)
PLATE_V137 = [[19.535, 27.485], [19.198, 27.748], [18.513, 28.26], [18.165, 28.509], [17.807, 28.743], [17.081, 29.194], [16.713, 29.412], [16.336, 29.614], [15.574, 30.002], [15.188, 30.188], [14.795, 30.357], [14.003, 30.678], [13.603, 30.831], [13.197, 30.966], [12.38, 31.218], [11.969, 31.335], [11.553, 31.435], [10.717, 31.617], [10.297, 31.699], [9.874, 31.763], [9.026, 31.872], [8.601, 31.918], [8.174, 31.945], [7.106, 31.991], [6.678, 32.0], [-6.678, 32.0], [-7.106, 31.991], [-8.174, 31.945], [-8.601, 31.918], [-9.026, 31.872], [-9.874, 31.763], [-10.297, 31.699], [-10.717, 31.617], [-11.553, 31.435], [-11.969, 31.335], [-12.38, 31.218], [-13.197, 30.966], [-13.603, 30.831], [-14.003, 30.678], [-14.795, 30.357], [-15.188, 30.188], [-15.574, 30.002], [-16.336, 29.614], [-16.713, 29.412], [-17.081, 29.194], [-17.807, 28.743], [-18.165, 28.509], [-18.513, 28.26], [-19.198, 27.748], [-19.535, 27.485], [-19.861, 27.207], [-20.659, 26.496], [-20.973, 26.205], [-25.656, 21.661], [-27.126, 20.151], [-27.591, 19.637], [-27.911, 19.245], [-28.21, 18.837], [-28.489, 18.414], [-28.745, 17.977], [-28.98, 17.529], [-29.191, 17.069], [-29.378, 16.598], [-29.542, 16.119], [-29.681, 15.632], [-29.796, 15.139], [-29.885, 14.641], [-29.949, 14.139], [-29.987, 13.634], [-30.0, 13.128], [-30.0, -20.545], [-29.988, -21.029], [-29.953, -21.511], [-29.895, -21.992], [-29.813, -22.468], [-29.709, -22.941], [-29.582, -23.407], [-29.432, -23.867], [-29.26, -24.319], [-29.067, -24.763], [-28.853, -25.196], [-16.324, -49.045], [-15.93, -49.762], [-15.423, -50.405], [-14.819, -50.956], [-14.133, -51.402], [-13.383, -51.73], [-12.59, -51.931], [-11.774, -52.0], [11.762, -52.0], [12.579, -51.933], [13.374, -51.733], [14.126, -51.405], [14.815, -50.96], [15.421, -50.407], [15.929, -49.763], [16.324, -49.045], [28.853, -25.196], [29.067, -24.763], [29.261, -24.319], [29.432, -23.867], [29.582, -23.407], [29.709, -22.941], [29.813, -22.468], [29.895, -21.992], [29.953, -21.512], [29.988, -21.029], [30.0, -20.546], [30.0, 13.128], [29.987, 13.634], [29.949, 14.139], [29.885, 14.641], [29.796, 15.139], [29.681, 15.632], [29.542, 16.119], [29.378, 16.598], [29.191, 17.069], [28.98, 17.529], [28.745, 17.977], [28.489, 18.414], [28.21, 18.837], [27.911, 19.245], [27.537, 19.7], [27.126, 20.151], [25.656, 21.661], [20.973, 26.205], [20.659, 26.496]];

RING_C = [0.0, 0.0];
RING_OD = 55.58;
RING_ID = 44.48;
RING_DEPTH = 0.6;
STRIP_W = 7.0;
STRIP_L = 18.0;
STRIP_CLEAR = 0.25;
STRIP_CY = RING_C[1] - 40.225;
STRIP_DEPTH = 0.6;
STRIP_CHAMFER = 0.3;

WALL_T = 2.0;
FRONT_Y = -52.0;               // outer face of the USB end (= v1.37.0 plate front)
USB_FACE_RECESS = 0.0;         // USB-C face behind the front face (0 = flush)
BOX_CORNER_R = 2.0;
BOX_FRONT_R = 1.0;
PCB_CLEAR = 0.15;
POST_L = 1.5;
POST_W = 2.0;
POST_ABOVE = 0.8;
CRUSH_YS = [PCB_L - 4.0, PCB_L - 9.0];        // from the USB-end PCB edge
CRUSH_R = 0.6;
CRUSH_IFR = 0.1;               // interference per side
CRUSH_LEADIN = 0.5;

COLLAR_H = 1.8;
ANT_SLOT_H = round((FPC_T + 0.27) * 100) / 100;   // lid underside -> tab lip (0.5)
ANT_CLEAR = 0.3;
ANT_FRONT_GAP = 1.0;           // tongue slots -> FPC front edge (room for the nubs)
TAB_COUNT = 3;                 // tabs per side
TAB_L = 6.0;
TAB_LIP_W = 1.6;               // lip under the FPC edge (1.3 mm overlap)
TAB_LIP_T = COLLAR_H - ANT_SLOT_H;   // lip down to the collar bottom (1.3)
TAB_LEDGE_W = 0.8;             // flat seat for the FPC edge at the collar face
TAB_SLOPE_DEG = 45.0;          // slope from the ledge to the full lip width
TAB_SLOPE_DZ = (TAB_LIP_W - TAB_LEDGE_W) * tan(TAB_SLOPE_DEG);
ANT_STOP_H = 0.3;              // ramp nubs in front of the FPC (0 disables)
ANT_STOP_L = 0.8;
ANT_STOP_XS = [[-4.0, -2.0], [2.0, 4.0]];

POCKET_FLOOR_Z = PLATE_T;
BOARD_Z = POCKET_FLOOR_Z;
LID_CLEAR = 0.3;

USB_OPEN_W = 10.0;
USB_OPEN_H = 4.1;
USB_CZ = BOARD_Z + PCB_T + USB_BODY_H / 2;
USB_Z0 = round((USB_CZ - USB_OPEN_H / 2) * 100) / 100;
USB_Z1 = round((USB_CZ + USB_OPEN_H / 2) * 100) / 100;
USB_TOP_WALL = 0.8;
USB_TOP_Z = BOARD_Z + PCB_T + USB_BODY_H;
USB_PAD_HX = 3.0;
USB_PAD_PRELOAD = -0.35;       // < 0: gap pad -> shell (was 0.05, pad 2 x 0.2 thinner)
// T-shaped pressure pad on the RF shield: band in front of the tongue hinges
// plus a strip on the web between the two tongues
SHIELD_PAD_PRELOAD = -0.35;    // same nominal gap as the USB-C pad (which fits)
SHIELD_PAD_INSET = 0.5;
SHIELD_PAD_SLOT_CLEAR = 0.25;
SHIELD_PAD_HINGE_CLEAR = 0.3;

WALL_TOP = round(max(BOARD_Z + TALL_H + LID_CLEAR, USB_Z1 + USB_TOP_WALL) * 100) / 100;
LID_T = 0.8;
LID_EDGE_R = 0.5;
LID_EDGE_STEPS = 8;
COLLAR_T = 1.2;
COLLAR_CLEAR = 0.1;
SNAP_COUNT_SIDE = 3;
FRONT_SNAP_COUNT = 2;          // snap bumps per side wall over the PCB
PCB_RELIEF = 0.6;              // PCB pocket walls stepped out above the PCB
PCB_RELIEF_DZ = 0.7;           // relief starts this far above the PCB top
SNAP_BACK = true;
SNAP_LEN = 8.0;
SNAP_R = 0.6;
SNAP_PROTRUDE = 0.4;
SNAP_GROOVE_CLEAR = 0.1;
SNAP_Z = WALL_TOP - 0.85;

TONGUE_W = 5.0;
TONGUE_FREE = 1.5;             // free end beyond the plunger centre (far end)
TONGUE_FLEX = 11.0;            // hinge line in front of the plunger centre (8.0 strained PLA)
SLOT_W = 0.5;
PLUNGER_D = 2.0;
BTN_GAP = 1.0;                 // plunger underside -> button top (0.2 triggered on light touch)
// button labels as in pcb_box_case.py v1.37.0: DejaVu Sans Condensed Bold,
// centred on the tongue, filling 67.4 % of its width / 85.5 % of its length
// (aspect kept, the tighter limit wins)
BTN_FILL_W = 0.674;
BTN_FILL_L = 0.855;
BTN_LABEL_ASPECT = 4.0938;     // "RESET" glyph bbox length / height in this font (measured in Python)
BTN_MARK_BOLD = 0.0;           // extra stroke weight: outline offset per side
BTN_MARK_DEPTH = 0.4;
BTN_FONT = "DejaVu Sans Condensed:style=Bold";

CAMERA_H = 4.0;
CAMERA_PATH = [[-8.0, 5.0], [-5.0, 5.0], [-3.0, 8.0], [3.0, 8.0], [5.0, 5.0], [8.0, 5.0], [8.2588, 4.9659], [8.5, 4.866], [8.7071, 4.7071], [8.866, 4.5], [8.9659, 4.2588], [9.0, 4.0], [9.0, -7.0], [8.9659, -7.2588], [8.866, -7.5], [8.7071, -7.7071], [8.5, -7.866], [8.2588, -7.9659], [8.0, -8.0], [-8.0, -8.0], [-8.2588, -7.9659], [-8.5, -7.866], [-8.7071, -7.7071], [-8.866, -7.5], [-8.9659, -7.2588], [-9.0, -7.0], [-9.0, 4.0], [-8.9659, 4.2588], [-8.866, 4.5], [-8.7071, 4.7071], [-8.5, 4.866], [-8.2588, 4.9659]];

// [text, cap height, centre x, centre y (undef = over the antenna), rotation]
LID_TEXTS = [["wlanscout+", 4.4, 0.0, undef, 90]];
LID_TEXT_DEPTH = 0.3;
LID_FONT = "DejaVu Sans:style=Bold";

SIDE_WIDTH = 6.0;
SIDE_STEPS = 32;
FLANK_TRIM_SLOPE = 1.0;
BIG = 500.0;

// --- derived layout --------------------------------------------------------
PCB_Y0 = FRONT_Y + USB_OVERHANG + USB_FACE_RECESS;
Y_IN0 = PCB_Y0 - PCB_CLEAR;
PCB_Y1 = PCB_Y0 + PCB_L;
Y_SEC = PCB_Y1 + PCB_CLEAR;
TONGUE_Y1 = len(BUTTONS) > 0 ? max([for (b = BUTTONS) PCB_Y0 + b[2] + TONGUE_FREE]) : PCB_Y1;
ANT_Y0 = max(TONGUE_Y1 + SLOT_W + ANT_FRONT_GAP, Y_SEC);
ANT_Y1 = ANT_Y0 + ANT_L;
Y_IN1 = ANT_Y1 + ANT_CLEAR + COLLAR_T + COLLAR_CLEAR;   // collar end = FPC end stop
BACK_Y = Y_IN1 + WALL_T;
PCB_HX = PCB_W / 2 + PCB_CLEAR;
ANT_HX = ANT_W / 2 + ANT_CLEAR;
CAB_HX = ANT_HX + COLLAR_T + COLLAR_CLEAR;     // cable space (wall inner face)
OUT_HX = max(PCB_HX, CAB_HX) + WALL_T;
COLLAR_Y0 = Y_SEC + POST_L + 0.3;
SH_X0 = -PCB_W / 2 + SHIELD_EDGE_L;           // RF shield outline
SH_X1 = PCB_W / 2 - SHIELD_EDGE_R;
SH_Y0 = PCB_Y0 + USB_LEN - USB_OVERHANG + SHIELD_GAP;
SH_Y1 = SH_Y0 + SHIELD_L;
SHIELD_TOP_Z = BOARD_Z + SHIELD_H;
UP_HX = PCB_HX + PCB_RELIEF;                  // PCB pocket half width above the relief
RELIEF_Z = BOARD_Z + PCB_T + PCB_RELIEF_DZ;
TOP_Z = WALL_TOP + LID_T;
TAB_Y0 = max(ANT_Y0, COLLAR_Y0) + 2.0 + TAB_L / 2;
TAB_Y1 = ANT_Y1 - 2.0 - TAB_L / 2;
TAB_YS = TAB_COUNT > 1 ? [for (i = [0 : TAB_COUNT - 1]) TAB_Y0 + (TAB_Y1 - TAB_Y0) * i / (TAB_COUNT - 1)]
                       : [(TAB_Y0 + TAB_Y1) / 2];

if (!MEASURED)
    echo("WARNING: UNVERIFIED assumptions in use - check before printing");
if (USB_PAD_Y[1] > USB_LEN - USB_OVERHANG || USB_PAD_Y[0] < 0)
    echo("LAYOUT: USB-C pad not on the on-board part of the receptacle");
echo(str("box outer: x +-", OUT_HX, ", y ", FRONT_Y, " .. ", BACK_Y,
         ", WALL_TOP=", WALL_TOP, ", USB opening z ", USB_Z0, " .. ", USB_Z1,
         ", lid ", LID_T, " thick, FPC y ", ANT_Y0, " .. ", ANT_Y1, ", tabs at y ", TAB_YS));
if (UP_HX - (SNAP_PROTRUDE + SNAP_GROOVE_CLEAR) < PCB_W / 2 + 0.1)
    echo("LAYOUT: front snap bumps block the PCB insertion");
if (PLUNGER_D > min(BTN_W, BTN_L) - 0.3) echo("LAYOUT: plunger wider than the button cap");
if (Y_IN0 - FRONT_Y < 1.2) echo("LAYOUT: front wall thinner than 1.2 mm");
if (TAB_SLOPE_DZ > TAB_LIP_T + 1e-6) echo("LAYOUT: tab slope does not reach the full lip width within the collar height");
if (FEED_W > 2 * (ANT_HX - TAB_LIP_W) - 1.0) echo("LAYOUT: feed joint does not pass between the tab lips");

// --- 2D helpers --------------------------------------------------------------
module rect(x0, y0, x1, y1) { translate([x0, y0]) square([x1 - x0, y1 - y0]); }

module rounded_rect(x0, y0, x1, y1, rf, rb) {
    ym = (y0 + y1) / 2;
    intersection() {
        union() {
            offset(r = rf) rect(x0 + rf, y0 + rf, x1 - rf, ym);
            offset(r = rb) rect(x0 + rb, ym, x1 - rb, y1 - rb);
            rect(x0, y0 + rf, x1, y1 - rb);
        }
        rect(x0, y0, x1, y1);
    }
}

// box footprint; with PLATE_SHAPE "v137" cut to the plate outline at the USB end
module box_outer() {
    if (PLATE_SHAPE == "v137")
        intersection() {
            rounded_rect(-OUT_HX, FRONT_Y, OUT_HX, BACK_Y, BOX_FRONT_R, BOX_CORNER_R);
            polygon(PLATE_V137);
        }
    else rounded_rect(-OUT_HX, FRONT_Y, OUT_HX, BACK_Y, BOX_FRONT_R, BOX_CORNER_R);
}

module box_inner() {
    rect(-PCB_HX, Y_IN0, PCB_HX, Y_SEC + 0.01);
    rect(-CAB_HX, Y_SEC, CAB_HX, Y_IN1);
}

module strip_shape(extra = 0) {
    w = STRIP_W / 2 + STRIP_CLEAR;
    l = STRIP_L / 2 + STRIP_CLEAR;
    hull() for (s = [-1, 1]) translate([0, STRIP_CY + s * (l - w)]) circle(r = w + extra, $fn = 128);
}

// convex hull of ring, strip and box, cut flush with the USB end face
module plate_outline() {
    intersection() {
        hull() {
            if (PLATE_SHAPE == "v137") polygon(PLATE_V137);
            else {
                translate(RING_C) circle(d = RING_OD + 2 * PLATE_RING_MARGIN, $fn = 192);
                strip_shape(PLATE_STRIP_MARGIN);
            }
            box_outer();
        }
        rect(-BIG, FRONT_Y, BIG, BIG);
    }
}

module camera_icon() {
    scale(CAMERA_H / 18) {
        difference() {
            offset(r = 1, $fn = 24) polygon(CAMERA_PATH);
            offset(delta = -1) polygon(CAMERA_PATH);
        }
        translate([0, -1]) difference() { circle(r = 4.5); circle(r = 2.5); }
    }
}

// text scaled to the given overall height, centred on [cx, cy]
module label(txt, h, cx, cy, rot, font) {
    translate([cx, cy]) rotate(rot)
        resize([0, h], auto = true)
            text(txt, size = 10, font = font, halign = "center", valign = "center");
}

// --- 3D helpers --------------------------------------------------------------
module slab(z) { translate([0, 0, z - 0.001]) linear_extrude(0.001) children(); }

// plate with a quarter-round fillet on its top outer edge (convex outline)
module rounded_plate(z0, z1, r) {
    n = max(LID_EDGE_STEPS, floor(12 * r) + 1);
    zb = z1 - r;
    translate([0, 0, z0]) linear_extrude(zb - z0 + 0.001) children();
    for (i = [0 : n - 1]) hull() {
        a0 = 90 * i / n;
        a1 = 90 * (i + 1) / n;
        translate([0, 0, zb + r * sin(a0)]) linear_extrude(0.001) offset(r = -r * (1 - cos(a0))) children();
        slab(zb + r * sin(a1)) offset(r = -r * (1 - cos(a1))) children();
    }
}

module ycyl(x, y, z, r, len) { translate([x, y, z]) rotate([90, 0, 0]) cylinder(r = r, h = len, center = true); }
module xcyl(x, y, z, r, len) { translate([x, y, z]) rotate([0, 90, 0]) cylinder(r = r, h = len, center = true); }
// --- flanks ------------------------------------------------------------------
function flank_offset(z) =
    let(h = WALL_TOP - PLATE_T, w = SIDE_WIDTH)
    side_profile == "convex"
        ? let(q = min(max((z - PLATE_T) / h, 0), 1)) w * sqrt(1 - q * q)
        : let(q = min(max((WALL_TOP - z) / h, 0), 1)) w * (1 - sqrt(1 - q * q));

function flank_zs() =
    let(h = WALL_TOP - PLATE_T, n = SIDE_STEPS,
        zs = side_profile == "convex"
            ? [for (i = [0 : n]) PLATE_T + h * sin(90 * i / n)]
            : [for (i = [0 : n]) WALL_TOP - h * sin(90 * (n - i) / n)])
    [for (i = [0 : n]) i == 0 ? zs[0] - 0.01 : zs[i]];

module flank_section(z) {
    d = flank_offset(z);
    trim = PLATE_EDGE_R + max(0, z - PLATE_T) * FLANK_TRIM_SLOPE;
    intersection() {
        if (d > 1e-3) offset(r = d, $fn = 96) box_outer(); else box_outer();
        offset(r = -trim, $fn = 96) plate_outline();
    }
}

module flank() {
    zs = flank_zs();
    difference() {
        intersection() {
            for (i = [0 : len(zs) - 2]) hull() {
                translate([0, 0, zs[i]]) linear_extrude(0.001) flank_section(zs[i]);
                slab(zs[i + 1]) flank_section(zs[i + 1]);
            }
            translate([0, 0, PLATE_T - 0.02]) linear_extrude(WALL_TOP - 0.3 - PLATE_T + 0.02)
                offset(delta = -0.05) plate_outline();
        }
        translate([0, 0, -1]) linear_extrude(WALL_TOP + 2) offset(r = -0.02) box_outer();
    }
}

// --- recesses / openings -------------------------------------------------------
module strip_recess() {
    c = STRIP_CHAMFER;
    translate([0, 0, -1]) linear_extrude(1 + STRIP_DEPTH) strip_shape();
    hull() {   // 45 deg chamfer at the bed-side edge
        translate([0, 0, -0.5]) linear_extrude(0.5) offset(r = c) strip_shape();
        slab(c) strip_shape();
    }
}

module ring_recess() {
    translate([RING_C[0], RING_C[1], -1]) linear_extrude(1 + RING_DEPTH)
        difference() { circle(d = RING_OD, $fn = 192); circle(d = RING_ID, $fn = 192); }
}

module usb_opening() {
    r = (USB_Z1 - USB_Z0) / 2;
    hull() for (x = [USB_X - USB_OPEN_W / 2 + r, USB_X + USB_OPEN_W / 2 - r])
        translate([x, FRONT_Y - 1, USB_Z0 + r]) rotate([-90, 0, 0])
            cylinder(r = r, h = Y_IN0 - FRONT_Y + 2);
}


function snap_ys() =
    let(y0 = COLLAR_Y0 + 1.0 + SNAP_LEN / 2,
        y1 = Y_IN1 - COLLAR_CLEAR - COLLAR_T - 1.0 - SNAP_LEN / 2)
    SNAP_COUNT_SIDE > 1 ? [for (i = [0 : SNAP_COUNT_SIDE - 1]) y0 + (y1 - y0) * i / (SNAP_COUNT_SIDE - 1)]
                        : [(y0 + y1) / 2];

// snap bumps (extra = 0) or grooves (extra = SNAP_GROOVE_CLEAR), behind the
// entry opening (sides) and on the far-end wall
function front_snap_ys() =
    let(y0 = Y_IN0 + 1.0 + SNAP_LEN / 2, y1 = Y_SEC - 1.0 - SNAP_LEN / 2)
    FRONT_SNAP_COUNT > 1 ? [for (i = [0 : FRONT_SNAP_COUNT - 1]) y0 + (y1 - y0) * i / (FRONT_SNAP_COUNT - 1)]
                         : FRONT_SNAP_COUNT == 1 ? [(y0 + y1) / 2] : [];

module snaps(extra) {
    r = SNAP_R + extra;
    len = SNAP_LEN + 2 * extra;
    off = SNAP_PROTRUDE - SNAP_R;
    for (sx = [-1, 1], y = snap_ys()) ycyl(sx * (CAB_HX + off), y, SNAP_Z, r, len);
    for (sx = [-1, 1], y = front_snap_ys()) ycyl(sx * (UP_HX + off), y, SNAP_Z, r, len);
    if (SNAP_BACK) xcyl(0, Y_IN1 + off, SNAP_Z, r, len);
}

// --- buttons (tongue hinge towards the USB end, at t[1]) -------------------------
function btn_y(b) = PCB_Y0 + b[2];
function tongue(b) = [b[1] - TONGUE_W / 2, btn_y(b) - TONGUE_FLEX,
                      b[1] + TONGUE_W / 2, btn_y(b) + TONGUE_FREE];

module tongue_slot(b) {
    t = tongue(b);
    intersection() {
        difference() {
            offset(r = SLOT_W) rect(t[0], t[1] - 2, t[2], t[3]);
            rect(t[0], t[1] - 3, t[2], t[3]);   // open at the hinge end, no sliver
        }
        rect(-BIG, t[1], BIG, BIG);
    }
}

module shield_pad_shape() {
    hinge = min([for (b = BUTTONS) tongue(b)[1]]);
    xs = [for (b = BUTTONS) b[1]];
    i = SHIELD_PAD_INSET;
    c = SLOT_W + SHIELD_PAD_SLOT_CLEAR;
    x0 = min(xs) + TONGUE_W / 2 + c;
    x1 = max(xs) - TONGUE_W / 2 - c;
    rect(SH_X0 + i, SH_Y0 + i, SH_X1 - i, hinge - SHIELD_PAD_HINGE_CLEAR);
    if (len(BUTTONS) >= 2 && x1 - x0 > 0.8)
        rect(x0, hinge - SHIELD_PAD_HINGE_CLEAR - 0.01, x1, SH_Y1 - i);
}

module button_mark(b) {
    t = tongue(b);
    cy = (t[1] + t[3]) / 2;
    h = min(BTN_FILL_W * (t[2] - t[0]), BTN_FILL_L * (t[3] - t[1]) / BTN_LABEL_ASPECT);
    if (b[0] == CAMERA) translate([b[1], cy - 1.0]) rotate(90) camera_icon();
    else translate([b[1], cy]) rotate(90) offset(r = BTN_MARK_BOLD, $fn = 16)
        resize([0, h], auto = true)
            text(b[0], size = 10, font = BTN_FONT, halign = "center", valign = "center");
}

// --- parts ---------------------------------------------------------------------
module bottom() {
    post_z1 = BOARD_Z + PCB_T + POST_ABOVE;
    z_top = BOARD_Z + PCB_T;
    difference() {
        union() {
            rounded_plate(0, PLATE_T, PLATE_EDGE_R) plate_outline();
            linear_extrude(PLATE_T + 0.01) box_outer();   // straight USB end face
            difference() {
                translate([0, 0, PLATE_T - 0.01]) linear_extrude(WALL_TOP - PLATE_T + 0.01)
                    difference() { box_outer(); box_inner(); }
                // relief above the PCB: room for the front collar legs and snaps
                translate([0, 0, RELIEF_Z]) linear_extrude(WALL_TOP + 1 - RELIEF_Z)
                    rect(-UP_HX, Y_IN0, UP_HX, Y_SEC + 0.01);
            }
            // PCB end-stop posts at the far PCB corners
            for (sx = [-1, 1]) {
                xa = min(sx * (PCB_W / 2 - POST_W), sx * (CAB_HX + 0.01));
                xb = max(sx * (PCB_W / 2 - POST_W), sx * (CAB_HX + 0.01));
                translate([xa, Y_SEC, PLATE_T - 0.01]) cube([xb - xa, POST_L, post_z1 - PLATE_T + 0.01]);
            }
            // crush ribs on the PCB pocket long sides, conical lead-in on top
            for (sx = [-1, 1], dy = CRUSH_YS)
                translate([sx * (PCB_W / 2 - CRUSH_IFR + CRUSH_R), PCB_Y0 + dy, 0]) {
                    translate([0, 0, PLATE_T - 0.01]) cylinder(r = CRUSH_R, h = z_top - PLATE_T + 0.01, $fn = 32);
                    intersection() {
                        translate([0, 0, z_top - 0.01]) cylinder(r1 = CRUSH_R, r2 = 0, h = 2 * CRUSH_R, $fn = 32);
                        translate([-1, -1, z_top - 0.02]) cube([2, 2, CRUSH_LEADIN + 0.02]);
                    }
                }
            snaps(0);
            flank();
        }
        ring_recess();
        strip_recess();
        usb_opening();
    }
}

module lid() {
    difference() {
        union() {
            difference() {
                union() {
                    rounded_plate(WALL_TOP, TOP_Z, LID_EDGE_R) box_outer();
                    // U-shaped collar behind the PCB (sides + far end); its inner
                    // face guides the FPC edges, the far segment is the end stop
                    translate([0, 0, WALL_TOP - COLLAR_H]) linear_extrude(COLLAR_H + 0.01)
                        difference() {
                            rect(-CAB_HX + COLLAR_CLEAR, COLLAR_Y0, CAB_HX - COLLAR_CLEAR, Y_IN1 - COLLAR_CLEAR);
                            rect(-ANT_HX, COLLAR_Y0 - 1, ANT_HX, Y_IN1 - COLLAR_CLEAR - COLLAR_T);
                        }
                    // collar legs over the PCB, in the relief of the PCB pocket walls
                    translate([0, 0, WALL_TOP - COLLAR_H]) linear_extrude(COLLAR_H + 0.01)
                        for (sx = [-1, 1])
                            rect(min(sx * (UP_HX - COLLAR_CLEAR), sx * (UP_HX - COLLAR_CLEAR - COLLAR_T)), Y_IN0 + COLLAR_CLEAR,
                                 max(sx * (UP_HX - COLLAR_CLEAR), sx * (UP_HX - COLLAR_CLEAR - COLLAR_T)), Y_SEC);
                    // FPC tabs: lips on the collar inner faces, ANT_SLOT_H below the
                    // lid; flat ledge at the collar, then a slope to the full width
                    for (y = TAB_YS, sx = [-1, 1])
                        translate([0, y + TAB_L / 2, 0]) rotate([90, 0, 0]) linear_extrude(TAB_L)
                            scale([sx, 1]) polygon([
                                [ANT_HX + 0.01, WALL_TOP - ANT_SLOT_H],
                                [ANT_HX - TAB_LEDGE_W, WALL_TOP - ANT_SLOT_H],
                                [ANT_HX - TAB_LIP_W, WALL_TOP - ANT_SLOT_H - TAB_SLOPE_DZ],
                                [ANT_HX - TAB_LIP_W, WALL_TOP - ANT_SLOT_H - TAB_LIP_T],
                                [ANT_HX + 0.01, WALL_TOP - ANT_SLOT_H - TAB_LIP_T]]);
                    // ramp nubs in front of the FPC: steep face towards the FPC
                    if (ANT_STOP_H > 0)
                        for (xs = ANT_STOP_XS)
                            translate([xs[0], 0, 0]) rotate([90, 0, 90]) linear_extrude(xs[1] - xs[0])
                                polygon([[ANT_Y0 - 0.05 - ANT_STOP_L, WALL_TOP + 0.01],
                                         [ANT_Y0 - 0.05, WALL_TOP + 0.01],
                                         [ANT_Y0 - 0.05, WALL_TOP - ANT_STOP_H]]);
                    // T-shaped pressure pad on the RF shield
                    translate([0, 0, SHIELD_TOP_Z - SHIELD_PAD_PRELOAD])
                        linear_extrude(WALL_TOP + 0.01 - (SHIELD_TOP_Z - SHIELD_PAD_PRELOAD))
                            shield_pad_shape();
                    // pressure pad on the USB-C shell
                    translate([USB_X - USB_PAD_HX, PCB_Y0 + USB_PAD_Y[0], USB_TOP_Z - USB_PAD_PRELOAD])
                        cube([2 * USB_PAD_HX, USB_PAD_Y[1] - USB_PAD_Y[0],
                              WALL_TOP + 0.01 - (USB_TOP_Z - USB_PAD_PRELOAD)]);
                }
                snaps(SNAP_GROOVE_CLEAR);
                // tongue slots
                for (b = BUTTONS)
                    translate([0, 0, WALL_TOP - 1]) linear_extrude(LID_T + 2) tongue_slot(b);
            }
            // plungers
            for (b = BUTTONS) {
                z0 = BOARD_Z + b[3] + BTN_GAP;
                translate([b[1], btn_y(b), z0]) cylinder(d = PLUNGER_D, h = WALL_TOP + 0.01 - z0);
            }
        }
        translate([0, 0, TOP_Z - BTN_MARK_DEPTH]) linear_extrude(BTN_MARK_DEPTH + 1)
            for (b = BUTTONS) button_mark(b);
        translate([0, 0, TOP_Z - LID_TEXT_DEPTH]) linear_extrude(LID_TEXT_DEPTH + 1)
            for (t = LID_TEXTS)
                label(t[0], t[1], t[2], t[3] == undef ? (ANT_Y0 + ANT_Y1) / 2 : t[3], t[4], LID_FONT);
    }
}

if (part == "bottom") bottom();
else if (part == "lid") lid();
else if (part == "lid_print") translate([0, 0, TOP_Z]) rotate([0, 180, 0]) lid();
else if (part == "assembly") { bottom(); color("orange", 0.6) lid(); }
