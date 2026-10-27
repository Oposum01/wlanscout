// SPDX-License-Identifier: CC-BY-NC-SA-4.0
// SPDX-FileCopyrightText: Dtmcnamara (the original case)
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>
//
// Adapted from "Biscuit - DIY MagSafe case, ESP32-C5 Waveshare" by
// Dtmcnamara (https://makerworld.com/de/@Dtmcnamara),
// https://makerworld.com/de/models/2590419-biscuit-diy-magsafe-case-esp32-c5-waveshare
// licensed under CC BY-NC-SA 4.0,
// https://creativecommons.org/licenses/by-nc-sa/4.0/
// This file and the two STLs made from it are under the same licence,
// not under the GPL of the rest of wlanscout. Changed: rebuilt in
// OpenSCAD, the base plate 2.0 instead of 2.5 mm, the PCB box 8.4
// instead of 10.25 mm high, the outline 10 mm shorter and trimmed to
// 65 mm, the USB openings 0.5 mm lower, labels and button markings
// engraved.
//
// esp32-c5-magsafe-case-v6.scad
// ESP32-C5 MagSafe case: flat 2.0 mm base plate with MagSafe ring/slot
// recesses, PCB box with concave elliptical flanks, snap-fit lid with
// flexure buttons, engraved labels.
//
// Version: 1.14.0  (geometry equivalent to pcb_box_case.py v1.24.0)
//
// Coordinates: x across, y along the case (USB end at y = 48), z up from
// the underside of the base plate. Text is readable with the USB end
// facing the viewer.
//
// Render:  PART = "bottom" | "lid" | "assembly" | "exploded"
//   openscad -D 'PART="bottom"' -o esp32-c5-magsafe-case-base-v6.stl esp32-c5-magsafe-case-v6.scad
//   openscad -D 'PART="lid"'    -o esp32-c5-magsafe-case-lid-v6.stl  esp32-c5-magsafe-case-v6.scad

PART = "exploded";

/* [Quality] */
$fn = 64;
FLANK_STEPS = 32;              // loft segments of the elliptical flank

/* [Base plate] */
PLATE_T = 2.0;                 // was 2.5
PLATE_EDGE_R = 1.0;            // fillet radius of the base plate top edge
PLATE_EDGE_STEPS = 12;

// MagSafe recesses on the underside
RING_C = [0, 100];
RING_OD = 55.38;
RING_ID = 44.68;
RECESS_DEPTH = 0.6;            // ring recess depth

// alignment strip recess (straight piece of the MagSafe ring), strip 7 x 18 mm,
// stadium shape with fully rounded ends like the original slot
STRIP_W = 7.0;
STRIP_L = 18.0;
STRIP_CLEAR = 0.25;            // clearance per side
STRIP_CY = 59.775;             // recess centre (original + 1 mm towards the ring)
STRIP_DEPTH = 0.6;
STRIP_CHAMFER = 0.3;           // 45 deg chamfer at the bed-side edge (elephant foot)

/* [PCB box] */
POCKET = [-13.75, 50.0, 13.75, 113.0];   // x0, y0, x1, y1
POCKET_FLOOR = 1.5;
POCKET_CAVITY_Y1 = 112.6;      // original pocket floor ends 0.4 mm before the back wall
WALL_T = 2.0;
// battery connector incl. PCB is 6.36 mm high, PCB rests on the pocket floor
// (z = 1.5) -> top at 7.86; 0.54 mm clearance below the lid plate
WALL_TOP = 8.4;                // was 10.25
BOX_CORNER_R = 2.0;            // far-end outer corners

// USB openings in the front wall (stadium slots)
USB_OPENINGS = [[-11.26, -1.12], [1.26, 11.40]];  // x ranges
USB_Z = [3.0, 7.1];            // 0.5 mm lower than the original (3.5..7.6)

// PCB locating pins [x, y, diameter]
PINS = [[-10.8, 59.4, 1.4], [10.8, 59.4, 1.4], [-10.8, 109.6, 1.6], [10.8, 109.6, 1.6]];
PIN_TOP = 4.1;

/* [Flanks] */
SIDE_PROFILE = "concave";      // "concave" (cove) or "convex" (shoulder)
SIDE_WIDTH = 6.0;              // horizontal semi-axis of the ellipse
FLANK_TRIM_SLOPE = 1.0;        // where the flank reaches the plate edge (USB end) it is
                               // trimmed by a 45 deg surface rising from the edge fillet

/* [Snap fit] */
SNAP_Y = [65.0, 101.0];        // bumps on the side walls
SNAP_X_END = [0.0];            // bumps on front and back wall
SNAP_LEN = 8.0;
SNAP_R = 0.6;
SNAP_PROTRUDE = 0.4;
SNAP_Z = WALL_TOP - 0.85;
SNAP_FRONT = false;            // front wall bump would sit at USB opening height
SNAP_GROOVE_CLEAR = 0.1;

/* [Lid] */
LID_T = 1.0;
LID_EDGE_R = 0.5;              // fillet radius of the lid top edge
LID_EDGE_STEPS = 8;
COLLAR_Z0 = WALL_TOP - 1.8;    // collar 1.8 mm tall (was 1.5)
COLLAR_FRONT_SHORTEN = 0.2;    // front collar segment only (above the USB-C sockets)
COLLAR_T = 1.2;
COLLAR_CLEAR = 0.1;
BTN_X = 7.25;                  // plungers at +-BTN_X
BTN_Y = 62.2;
PLUNGER_D = 4.0;
PLUNGER_Z0 = 6.15;
TONGUE_W = 8.0;
TONGUE_Y = [56.0, 72.45];      // free end, hinge line
BTN_SLOT_W = 0.5;
SHIELD_PAD_C = [0, 94.05];     // pressure pad on the ESP32-C5 module shield
SHIELD_PAD_RAISE = 0.3;        // pad underside raised (pad shorter), z 6.6 -> 6.9

/* [Labels] */
FONT = "DejaVu Sans:style=Bold";
FRONT_LABELS = [["USB", -6.5], ["Serial", 6.5]];   // below the USB openings
FRONT_LABEL_H = 2.0;
FRONT_LABEL_Z = 1.5;
FRONT_LABEL_DEPTH = 0.4;
// [text, height, x, y, rotation]
LID_TEXTS = [["wlanscout+", 4.4, 0, 94.0, 90]];
LID_TEXT_DEPTH = 0.3;

// tactile finger dimple on the Snapshot button (spherical cap over the plunger)
DIMPLE_X = BTN_X;
DIMPLE_D = 6.0;
DIMPLE_DEPTH = 0;              // 0 = no dimple (was 0.35)

// button markings as in esp32c5_w5500_v31.scad (print better): "RESET" in
// DejaVu Sans Condensed Bold, cap height 2.5, and the camera pictogram 5.0 mm
// high, both 0.4 mm deep, centred on the tongues, rotated 90 deg like the
// other lid texts (reading from the USB end)
BTN_FONT = "DejaVu Sans Condensed:style=Bold";
BTN_LABEL = "RESET";
BTN_LABEL_H = 2.5;             // nominal value from v31; OpenSCAD renders the caps ~3.4 mm high
BTN_MARK_DEPTH = 0.4;
BTN_MARK_ROT = 90;
TONGUE_CY = (TONGUE_Y[0] + TONGUE_Y[1]) / 2;
CAMERA_H = 5.0;                // overall icon height incl. stroke
// camera path from snapshot-camera.svg, y up, centred on the viewBox centre
CAMERA_PATH = [[-8, 5], [-5, 5], [-3, 8], [3, 8], [5, 5], [8, 5], [8.2588, 4.9659], [8.5, 4.866],
  [8.7071, 4.7071], [8.866, 4.5], [8.9659, 4.2588], [9.0, 4.0], [9, -7], [8.9659, -7.2588],
  [8.866, -7.5], [8.7071, -7.7071], [8.5, -7.866], [8.2588, -7.9659], [8.0, -8.0], [-8, -8],
  [-8.2588, -7.9659], [-8.5, -7.866], [-8.7071, -7.7071], [-8.866, -7.5], [-8.9659, -7.2588],
  [-9.0, -7.0], [-9, 4], [-8.9659, 4.2588], [-8.866, 4.5], [-8.7071, 4.7071], [-8.5, 4.866],
  [-8.2588, 4.9659]];

EPS = 0.01;

// Outer contour of the case (from the original STL, shortened by 5 mm at the
// wide end and 5 mm at the USB end, sides trimmed to 65 mm width with R10
// transitions)
CASE_OUTLINE = [
    [-12.531, 48.064],
    [-13.383, 48.270],
    [-14.133, 48.598],
    [-14.819, 49.044],
    [-15.423, 49.595],
    [-15.930, 50.238],
    [-16.324, 50.955],
    [-31.353, 79.562],
    [-31.567, 79.996],
    [-31.760, 80.439],
    [-31.932, 80.892],
    [-32.082, 81.352],
    [-32.209, 81.818],
    [-32.313, 82.290],
    [-32.395, 82.767],
    [-32.453, 83.247],
    [-32.488, 83.730],
    [-32.500, 84.213],
    [-32.500, 109.894],
    [-32.488, 110.382],
    [-32.452, 110.870],
    [-32.393, 111.355],
    [-32.309, 111.837],
    [-32.203, 112.314],
    [-32.073, 112.785],
    [-31.920, 113.249],
    [-31.745, 113.706],
    [-31.548, 114.153],
    [-31.329, 114.590],
    [-31.089, 115.016],
    [-30.828, 115.430],
    [-30.343, 116.160],
    [-29.989, 116.658],
    [-28.994, 117.973],
    [-28.611, 118.448],
    [-27.537, 119.700],
    [-27.126, 120.151],
    [-25.872, 121.445],
    [-20.973, 126.205],
    [-19.535, 127.485],
    [-18.165, 128.509],
    [-17.081, 129.194],
    [-16.713, 129.412],
    [-15.574, 130.002],
    [-14.795, 130.357],
    [-13.603, 130.831],
    [-12.380, 131.218],
    [-11.969, 131.335],
    [-10.717, 131.617],
    [-9.874, 131.763],
    [-8.601, 131.918],
    [-7.106, 131.991],
    [7.106, 131.991],
    [8.601, 131.918],
    [9.874, 131.763],
    [10.297, 131.699],
    [11.553, 131.435],
    [12.380, 131.218],
    [13.603, 130.831],
    [14.795, 130.357],
    [15.574, 130.002],
    [16.713, 129.412],
    [17.807, 128.743],
    [18.513, 128.260],
    [19.535, 127.485],
    [20.659, 126.496],
    [25.656, 121.661],
    [27.126, 120.151],
    [27.537, 119.700],
    [28.611, 118.448],
    [28.994, 117.973],
    [29.989, 116.658],
    [30.343, 116.160],
    [31.089, 115.016],
    [31.329, 114.590],
    [31.548, 114.153],
    [31.745, 113.706],
    [31.920, 113.249],
    [32.073, 112.785],
    [32.203, 112.314],
    [32.309, 111.837],
    [32.393, 111.355],
    [32.452, 110.870],
    [32.488, 110.382],
    [32.500, 109.894],
    [32.500, 84.213],
    [32.488, 83.730],
    [32.453, 83.247],
    [32.395, 82.767],
    [32.313, 82.291],
    [32.209, 81.818],
    [32.082, 81.352],
    [31.932, 80.892],
    [31.761, 80.440],
    [31.567, 79.996],
    [31.353, 79.563],
    [16.324, 50.955],
    [15.929, 50.237],
    [15.421, 49.593],
    [14.815, 49.040],
    [14.126, 48.595],
    [13.374, 48.267],
    [12.579, 48.067],
    [11.762, 48.000],
    [-11.774, 48.000]
];

// ---------------------------------------------------------------- 2D shapes
module outline2d() { polygon(CASE_OUTLINE); }

module pocket2d() {
    translate([POCKET[0], POCKET[1]]) square([POCKET[2] - POCKET[0], POCKET[3] - POCKET[1]]);
}

module box_outer2d() {
    x0 = POCKET[0] - WALL_T; x1 = POCKET[2] + WALL_T; y1 = POCKET[3] + WALL_T;
    r = BOX_CORNER_R;
    intersection() {
        union() {
            translate([x0, 40]) square([x1 - x0, y1 - r - 40]);
            translate([x0 + r, y1 - r]) offset(r = r) square([x1 - x0 - 2 * r, EPS]);
            translate([x0 + r, 40]) square([x1 - x0 - 2 * r, y1 - 40]);
        }
        outline2d();
    }
}

module stadium2d(x0, x1, z0, z1) {
    r = (z1 - z0) / 2;
    hull() {
        translate([x0 + r, z0 + r]) circle(r = r);
        translate([x1 - r, z0 + r]) circle(r = r);
    }
}

module label2d(txt, h) {
    resize([0, h], auto = true)
        text(txt, font = FONT, size = 10, halign = "center", valign = "center");
}

// ---------------------------------------------------------------- bottom
function flank_offset(z) =
    let(h = WALL_TOP - PLATE_T, w = SIDE_WIDTH)
    SIDE_PROFILE == "convex"
        ? w * sqrt(max(0, 1 - pow((z - PLATE_T) / h, 2)))
        : w * (1 - sqrt(max(0, 1 - pow((WALL_TOP - z) / h, 2))));

function flank_z(i) =
    let(h = WALL_TOP - PLATE_T, t = 90 * i / FLANK_STEPS)
    SIDE_PROFILE == "convex" ? PLATE_T + h * sin(t)
                             : WALL_TOP - h * sin(90 - t);

module flank_slice(z) {
    d = flank_offset(z);
    trim = PLATE_EDGE_R + max(0, z - PLATE_T) * FLANK_TRIM_SLOPE;
    translate([0, 0, z])
        linear_extrude(EPS)
            intersection() {
                if (d > 0.001) offset(r = d) box_outer2d(); else box_outer2d();
                offset(r = -trim) outline2d();
            }
}

module flank() {
    difference() {
        intersection() {
            union() for (i = [0 : FLANK_STEPS - 1])
                hull() { flank_slice(flank_z(i)); flank_slice(flank_z(i + 1)); }
            translate([0, 0, PLATE_T - EPS]) linear_extrude(WALL_TOP - PLATE_T + EPS) outline2d();
        }
        translate([0, 0, -1]) linear_extrude(WALL_TOP + 2) box_outer2d();
    }
}

module snap_cyl(extra_r = 0, extra_len = 0) {
    r = SNAP_R + extra_r;
    l = SNAP_LEN + 2 * extra_len;
    off = SNAP_PROTRUDE - SNAP_R;
    for (s = [-1, 1], y = SNAP_Y) {            // side walls
        xw = s < 0 ? POCKET[0] : POCKET[2];
        translate([xw - s * off, y, SNAP_Z]) rotate([90, 0, 0]) cylinder(r = r, h = l, center = true);
    }
    for (s = SNAP_FRONT ? [-1, 1] : [1], x = SNAP_X_END) {   // front / back wall
        yw = s < 0 ? POCKET[1] : POCKET[3];
        translate([x, yw - s * off, SNAP_Z]) rotate([0, 90, 0]) cylinder(r = r, h = l, center = true);
    }
}

module strip2d() {
    w = STRIP_W + 2 * STRIP_CLEAR;
    l = STRIP_L + 2 * STRIP_CLEAR;
    hull() {
        translate([0, STRIP_CY - l / 2 + w / 2]) circle(d = w);
        translate([0, STRIP_CY + l / 2 - w / 2]) circle(d = w);
    }
}

module strip_recess() {
    translate([0, 0, -1]) linear_extrude(STRIP_DEPTH + 1) strip2d();
    hull() {                                  // chamfer at the underside edge
        translate([0, 0, -0.5]) linear_extrude(0.5) offset(r = STRIP_CHAMFER) strip2d();
        translate([0, 0, STRIP_CHAMFER - EPS]) linear_extrude(EPS) strip2d();
    }
}

module pin(d) {
    h = PIN_TOP - POCKET_FLOOR;
    cylinder(d1 = d + 0.6, d2 = d, h = 0.3);
    cylinder(d = d, h = h - 0.4);
    translate([0, 0, h - 0.4]) cylinder(d1 = d, d2 = d - 0.5, h = 0.4);
}

// base plate with a quarter-round fillet on its top edge (contour is convex)
module base_plate() {
    zb = PLATE_T - PLATE_EDGE_R;
    linear_extrude(zb + EPS) outline2d();
    for (i = [0 : PLATE_EDGE_STEPS - 1])
        hull() for (a = [90 * i / PLATE_EDGE_STEPS, 90 * (i + 1) / PLATE_EDGE_STEPS])
            translate([0, 0, zb + PLATE_EDGE_R * sin(a) - (a >= 90 ? EPS : 0)]) linear_extrude(EPS)
                offset(r = -PLATE_EDGE_R * (1 - cos(a))) outline2d();
}

module bottom() {
    face_y = min([for (p = CASE_OUTLINE) p[1]]);
    difference() {
        union() {
            base_plate();
            linear_extrude(WALL_TOP) box_outer2d();
            flank();
        }
        // pocket
        translate([0, 0, POCKET_FLOOR]) linear_extrude(PLATE_T - POCKET_FLOOR + EPS)
            translate([POCKET[0], POCKET[1]]) square([POCKET[2] - POCKET[0], POCKET_CAVITY_Y1 - POCKET[1]]);
        translate([0, 0, PLATE_T]) linear_extrude(WALL_TOP) pocket2d();
        // MagSafe ring + alignment slot (underside)
        translate([RING_C[0], RING_C[1], -1]) difference() {
            cylinder(d = RING_OD, h = RECESS_DEPTH + 1, $fn = 256);
            translate([0, 0, -1]) cylinder(d = RING_ID, h = RECESS_DEPTH + 3, $fn = 256);
        }
        strip_recess();
        // USB openings
        for (o = USB_OPENINGS)
            translate([0, face_y - 1, 0]) rotate([-90, 0, 0]) mirror([0, 1, 0])
                linear_extrude(WALL_T + 2) stadium2d(o[0], o[1], USB_Z[0], USB_Z[1]);
        // labels on the front face
        for (l = FRONT_LABELS)
            translate([l[1], face_y + FRONT_LABEL_DEPTH, FRONT_LABEL_Z]) rotate([90, 0, 0])
                linear_extrude(FRONT_LABEL_DEPTH + 1) label2d(l[0], FRONT_LABEL_H);
    }
    // locating pins + snap bumps
    for (p = PINS) translate([p[0], p[1], POCKET_FLOOR - EPS]) pin(p[2]);
    snap_cyl();
}

// ---------------------------------------------------------------- lid
module collar2d() {
    c = COLLAR_CLEAR;
    difference() {
        offset(r = 0.6) offset(delta = -0.6)
            translate([POCKET[0] + c, POCKET[1] + c])
                square([POCKET[2] - POCKET[0] - 2 * c, POCKET[3] - POCKET[1] - 2 * c]);
        offset(delta = -COLLAR_T) offset(r = 0.6) offset(delta = -0.6)
            translate([POCKET[0] + c, POCKET[1] + c])
                square([POCKET[2] - POCKET[0] - 2 * c, POCKET[3] - POCKET[1] - 2 * c]);
    }
}

module tongue2d(cx) {
    translate([cx - TONGUE_W / 2, TONGUE_Y[0]]) square([TONGUE_W, TONGUE_Y[1] - TONGUE_Y[0]]);
}

module button_slot2d(cx) {
    intersection() {
        difference() { offset(r = BTN_SLOT_W) tongue2d(cx); tongue2d(cx); }
        translate([-100, -100]) square([200, TONGUE_Y[1] + 100]);
    }
}

// shield pad with rounded lower edge: [z, width, length] (from the original lid,
// z values before SHIELD_PAD_RAISE)
SHIELD_PAD_PROFILE = [[6.6, 5.9, 9.78], [6.8, 7.08, 10.96], [7.0, 7.48, 11.36],
                     [7.5, 7.87, 11.75], [8.0, 7.9, 11.78], [WALL_TOP, 7.9, 11.78]];

module shield_pad_slice(p) {
    z = min(p[0] + SHIELD_PAD_RAISE, WALL_TOP);
    translate([SHIELD_PAD_C[0], SHIELD_PAD_C[1], z]) linear_extrude(EPS)
        offset(r = 1) square([p[1] - 2, p[2] - 2], center = true);
}

module shield_pad() {
    for (i = [0 : len(SHIELD_PAD_PROFILE) - 2])
        hull() { shield_pad_slice(SHIELD_PAD_PROFILE[i]); shield_pad_slice(SHIELD_PAD_PROFILE[i + 1]); }
}

module lid_plate() {
    zb = WALL_TOP + LID_T - LID_EDGE_R;
    translate([0, 0, WALL_TOP]) linear_extrude(LID_T - LID_EDGE_R + EPS) box_outer2d();
    for (i = [0 : LID_EDGE_STEPS - 1])
        hull() for (a = [90 * i / LID_EDGE_STEPS, 90 * (i + 1) / LID_EDGE_STEPS])
            translate([0, 0, zb + LID_EDGE_R * sin(a) - (a >= 90 ? EPS : 0)]) linear_extrude(EPS)
                offset(r = -LID_EDGE_R * (1 - cos(a))) box_outer2d();
}

module camera_icon2d() {                     // as camera_icon() in v31
    scale(CAMERA_H / 18) {                    // stroke outline spans 18 units in height
        difference() {                        // body, stroke 2 centred on the path
            offset(r = 1, $fn = 24) polygon(CAMERA_PATH);
            offset(delta = -1) polygon(CAMERA_PATH);
        }
        translate([0, -1]) difference() {     // lens: circle r 3.5 at (12, 13)
            circle(r = 4.5, $fn = 64);
            circle(r = 2.5, $fn = 64);
        }
    }
}

module button_marks2d() {
    translate([-BTN_X, TONGUE_CY]) rotate(BTN_MARK_ROT)
        text(BTN_LABEL, size = BTN_LABEL_H / 0.73, font = BTN_FONT, halign = "center", valign = "center");
    translate([BTN_X, TONGUE_CY]) rotate(BTN_MARK_ROT) camera_icon2d();
}

module lid() {
    top = WALL_TOP + LID_T;
    difference() {
        union() {
            lid_plate();
            translate([0, 0, COLLAR_Z0]) linear_extrude(WALL_TOP - COLLAR_Z0 + EPS) collar2d();
            for (s = [-1, 1])
                translate([s * BTN_X, BTN_Y, PLUNGER_Z0]) cylinder(d = PLUNGER_D, h = WALL_TOP - PLUNGER_Z0 + EPS);
            shield_pad();
        }
        snap_cyl(SNAP_GROOVE_CLEAR, SNAP_GROOVE_CLEAR);
        // front collar segment shorter (it would sit on the USB-C sockets)
        xi = POCKET[2] - COLLAR_CLEAR - COLLAR_T;
        translate([-xi, POCKET[1] - 1, COLLAR_Z0 - 1])
            cube([2 * xi, 1 + COLLAR_CLEAR + COLLAR_T + 0.01, 1 + COLLAR_FRONT_SHORTEN]);
        for (s = [-1, 1])
            translate([0, 0, WALL_TOP - 1]) linear_extrude(LID_T + 2) button_slot2d(s * BTN_X);
        // finger dimple
        dimple_r = DIMPLE_DEPTH > 0 ? (pow(DIMPLE_D / 2, 2) + pow(DIMPLE_DEPTH, 2)) / (2 * DIMPLE_DEPTH) : 0;
        if (DIMPLE_DEPTH > 0)
            translate([DIMPLE_X, BTN_Y, top + dimple_r - DIMPLE_DEPTH]) sphere(r = dimple_r, $fn = 256);
        // button markings (v31 style): RESET and camera pictogram
        translate([0, 0, top - BTN_MARK_DEPTH]) linear_extrude(BTN_MARK_DEPTH + 1) button_marks2d();
        for (t = LID_TEXTS)
            translate([t[2], t[3], top - LID_TEXT_DEPTH]) linear_extrude(LID_TEXT_DEPTH + 1)
                rotate(t[4]) label2d(t[0], t[1]);
    }
}

// ---------------------------------------------------------------- output
if (PART == "bottom") bottom();
else if (PART == "lid") lid();
else if (PART == "assembly") { color("lightgrey") bottom(); color("cornflowerblue") lid(); }
else { color("lightgrey") bottom(); color("cornflowerblue") translate([0, 0, 16]) lid(); }
