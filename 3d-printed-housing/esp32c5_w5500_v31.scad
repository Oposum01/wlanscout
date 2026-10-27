// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

// =====================================================================
//  Enclosure v31 - form-fitting, soldered wiring, connectors at the front,
//  tool-less snap-fit closure: the hood has a skirt that overlaps the tray
//  wall; latch beams in the skirt (fixed at both ends, bending in the layer
//  plane) snap over ramped bumps on the tray.
//   tray : pockets follow the PCB outlines; boards sit inset, ESP flush
//          with the rim, W5500 slightly recessed. The boards are joined only
//          by a short wire bridge behind the RJ45; the gap in front of and
//          behind it stays open (antenna side free).
//   hood : two identical plain bars (ESP and W5500), same length and height,
//          joined by a full-height bridge in the middle.
//   ESP32-C5 (Waveshare): component side DOWN, pins up, 2x USB-C front.
//   W5500 (Binghe): RJ45 at the front, 2x5 header at the rear; wires run
//          forward over the W5500 (away from the antenna) and cross to the
//          ESP behind the RJ45.
//  Parts: part = "tray" | "hood" | "assembly"
//  Units: mm
// =====================================================================

part = "assembly";

// ---------------- General ----------------
wall    = 2.8;
floor_t = 2.0;
roof_t  = 2.0;
hood_extra = 6.0;
// edge rounding
r_vert   = 3.0;     // vertical outer corners (plan view)
r_inner  = 2.0;     // concave corners where the bridge meets the bars
r_top    = 2.0;     // top edge of the hood
r_bot    = 1.2;     // bottom edge of the tray
r_steps  = 8;       // slices per rounded edge    // extra headroom inside the hood (cable connectors)
pclr    = 0.3;       // pocket clearance around each PCB
rf_clear = 15.0;     // antenna area -> W5500 copper (Espressif >= 15 mm)
bridge_l = 16.0;     // width of the wire bridge between the boards (wires stay >= 15 mm from the antenna)

// ---------------- ESP32-C5 Waveshare (measured by user) ----------------
esp_l = 61.5;  esp_w = 25.6;  esp_t = 1.6;   // antenna ends flush with the rear PCB edge
esp_comp_h = 3.3;           // tallest part on the component side
pad_h      = 5.0;           // carrier board above floor
esp_pad = 2.0;  esp_pad_front_x = 8.5;   // front pads behind the USB-C (clear of the button flexures)
// ESP buttons (RESET / BOOT) face the floor here (board flipped). Positions taken
// from the MagSafe case (BTN_X = +-7.25 from the PCB centre line, BTN_Y = 57.2
// with the PCB front edge at y = 45.8 -> 11.4 mm behind the front edge). VERIFY.
// Seen from the front: RESET right (-y), BOOT left (+y) - mirrored vs. MagSafe.
btn_dy    = 7.25;      // from the PCB centre line
btn_x     = 11.4;      // from the PCB front edge
btn_h     = 2.0;       // button height below the PCB bottom face
btn_gap   = 0.2;       // plunger top -> button
plunger_d = 3.0;
tongue_w  = 6.0;  tongue_l = 16.0;  tongue_t = 1.4;   // floor flexure, hinge at the rear
tongue_front = 2.5;    // tongue free end lies this far in front of the button (lever ~13.5 mm)
tongue_slot = 0.5;
// button markings engraved into the underside of the tongues; they read with
// the case upside down and the ports facing left
btn_mark_depth = 0.4;
btn_label_h = 2.5;     // cap height of "RESET"
esp_pin_h  = 8.5;           // header plastic + pin above PCB (clip pins to lower)
wire_sp    = 2.5;           // solder joint + wire bend above pin tips
mod_w = 18.0;
// USB-C openings taken from the MagSafe case (esp32-c5-magsafe-case-b-pcbbox):
// two stadium-shaped holes 10.14 x 4.1 mm (end radius 2.05), centres at
// -6.19 / +6.33 mm from the PCB centre, 2.0 mm wall. The board lies flipped
// here, so the port order is mirrored (+y = left seen from the front).
usb_pitch = 12.52;  usb_yoff = 0.07;
usb_open_w = 10.14;  usb_open_h = 4.1;
usb_proj = 0.8;  usb_body_h = 3.26;
// Rear retention: horizontal beam behind the PCB edge, spanning the full pocket
// width and anchored in both side walls (fixed-fixed). It flexes in the layer
// plane like the hood latches; strain ~12*t*lip/span^2.
clip_t = 1.2;  clip_lip = 0.7;  clip_lip_l = 14.0;      // ESP: lip centred on the beam
// ESP longitudinal fixing (USB-C plug forces): rigid stops front and rear,
// the spring beam only holds the board down
esp_stop_gap = 0.1;        // play per side between PCB edge and stop
esp_rstop_w = 1.5;         // rigid end blocks of the rear beam = rear stops (y width)
esp_beam_gap = 0.2;        // PCB edge -> beam front face (middle, free part)
usb_stop_margin = 0.4;     // front stop tabs keep this distance to the receptacles
w55_clip_t = 1.2;  w55_clip_lip = 0.6;                    // W5500: lip on the inner half only
clip_h = 2.8;                                             // beam height

// ---------------- W5500 Binghe (VERIFY) ----------------
w55_l = 55.0;  w55_w = 28.0;  w55_t = 1.6;
w55_stand  = 3.0;           // W5500 above floor
w55_comp_h = 3.0;           // tallest SMD part on top (chip, LDO, crystal)
rj45_w = 16.0;  rj45_l = 21.0;  rj45_h = 13.5;  rj45_yoff = 0.0;
// 2x5 header, oriented across the board (5 pins along the width), on the left
// long edge seen from the RJ45 front (= outer side, away from the ESP)
hdr_x_back = 1.5;  hdr_side = 1.0;  hdr_lx = 5.08;  hdr_ly = 12.7;
wire_d = 3.0;               // wire bundle height above the SMD parts
w55_pin_h = 8.5;

// ---------------- Fastening ----------------
// Skirt + latch beams. The beam is the skirt strip below a window; it is held
// at both ends and runs horizontally, so it bends along the printed layers
// (no load across layer lines). Strain ~ 12*t*d/L^2 (fixed-fixed beam).
skirt_t  = 1.6;     // skirt / beam thickness
skirt_h  = 5.8;     // skirt height below the rim
sgap     = 0.15;    // skirt <-> tray wall
beam_h   = 3.0;     // latch beam height (bottom strip of the skirt)
win_h    = 2.2;     // window height above the beam
latch_l  = 22.0;    // window length = free beam length (outer faces)
bump_d   = 0.6;     // bump height on the tray (interference = bump_d - sgap)
bump_l   = 16.0;    // bump length (outer faces)
bump_zh  = 1.6;     // bump height incl. lead-in ramp
finger_d = 4.0;

// ---------------- Lid text ----------------
txt1 = "wlanscout+";  txt2a = "DHCP";  txt2b = "or 192.168.1.1";
txt1_h = 9.0;  txt2_h = 5.0;     // target cap heights (width-limited if needed)
txt_maxw = 60;                   // max. text width on each roof
txt_font = "DejaVu Sans Condensed:style=Bold";
txt_depth = 0.8;
// port labels on the front face, above the USB-C openings
// (seen from the front: UART left = +y, USB right = -y)
port_labels = [["UART", 1], ["USB", -1]];
port_label_h = 2.5;        // cap height
port_label_gap = 1.2;      // opening top edge -> label bottom
port_label_depth = 0.4;

$fn = 40;

// ---------------- Derived layout (interior coordinates) ----------------
// USB-C receptacle front to outer face = 2.0 mm (as in the MagSafe case):
// the PCB edge sits on the pocket line, receptacles reach usb_proj into the wall
usb_wall = 2.0;
esp_x0 = usb_proj - (wall - usb_wall);         // = 0.0 with wall 2.8 / proj 0.8
esp_y0 = pclr;
ze     = floor_t + pad_h;                      // ESP carrier, bottom face
rim    = ze + esp_t;                           // tray rim = ESP top (flush)
esp_beam_x = esp_x0 + esp_l + esp_beam_gap;     // rear spring beam
esp_pl = esp_beam_x + clip_t + clip_lip + 0.2;  // + flex room behind the beam (= lip overlap + 0.2)
esp_pw = esp_w + 2*pclr;

ant_edge = esp_y0 + (esp_w + mod_w)/2;         // module edge on the W5500 side
w55_y0 = ant_edge + rf_clear;
ix     = max(esp_pl, w55_l + 2*pclr);
w55_x0 = pclr;                                 // RJ45 at the front wall
zw     = floor_t + w55_stand;
w55_top = zw + w55_t;

usb_zc = ze - usb_body_h/2;

// zone heights (inner ceiling z)
h_esp    = rim + esp_pin_h + wire_sp;
h_w55    = w55_top + w55_comp_h;
h_rj45   = w55_top + rj45_h + 0.5;
h_hdr    = w55_top + w55_pin_h + wire_sp;
h_chan   = max(w55_top + w55_comp_h + wire_d, rim + 3);
h_bridge = h_chan;
bridge_x0 = ix/2 - bridge_l/2;                 // bridge centred along the length
hdr_x0    = w55_x0 + w55_l - hdr_x_back - hdr_lx;
hdr_y0    = w55_y0 + w55_w - hdr_side - hdr_ly;

assert(pad_h >= esp_comp_h + 1, "pad_h too small");
assert(h_w55 > rim, "W5500 zone must clear the rim");

// 2D zones
module z_esp()    square([esp_pl, esp_pw]);
module z_w55()    translate([w55_x0 - pclr, w55_y0 - pclr]) square([w55_l + 2*pclr, w55_w + 2*pclr]);
// W5500 bar: same length as the ESP bar (open space behind the W5500 board)
module z_w55bar() translate([0, w55_y0 - pclr]) square([ix, w55_w + 2*pclr]);
module z_bridge() translate([bridge_x0, esp_pw - 0.01]) square([bridge_l, w55_y0 - pclr - esp_pw + 0.02]);
module z_rj45()   translate([w55_x0 - pclr - 0.01, w55_y0 + w55_w/2 + rj45_yoff - rj45_w/2 - 0.5])
                    square([pclr + rj45_l + 0.5, rj45_w + 1]);
module z_hdr()    clip_w55() translate([hdr_x0 - 2.5, hdr_y0 - 2.5]) square([hdr_lx + 5, hdr_ly + 5]);
// wire channel: header -> forward over the W5500 -> bridge (stays on the W5500 side)
// wire channel along the outer W5500 edge, then across behind the RJ45
module z_chan()   clip_w55() translate([bridge_x0, hdr_y0 - 2.5]) square([hdr_x0 - bridge_x0, hdr_ly + 5]);
module z_chan_x() clip_w55() translate([bridge_x0, w55_y0 - pclr - 0.01]) square([bridge_l, hdr_y0 - w55_y0 + pclr]);
module clip_w55() intersection() { children(); z_w55(); }
module z_all()    { z_esp(); z_w55bar(); z_bridge(); }

iy = w55_y0 + w55_w + pclr;
ox = ix + 2*wall;  oy = iy + 2*wall;

// latch positions on the outer faces: [x, y, dir] (dir: 0=+x, 1=+y, 2=-x, 3=-y)
// [x, y, dir, window length, bump height]
// inner faces (between the bars) are shorter -> shorter beam, less interference
latch_l_in = 17.0;  bump_d_in = 0.45;
notch_f = [0, bridge_x0];                                        // front notch, outer x range
notch_r = [bridge_x0 + bridge_l + 2*wall, ix + 2*wall];          // rear notch, outer x range
function snaps() = [
  [wall + 16, 0,  3, latch_l, bump_d],
  [wall + 46, 0,  3, latch_l, bump_d],
  [wall + 14, oy, 1, latch_l, bump_d],
  [wall + 40, oy, 1, latch_l, bump_d],
  // inner flanks: ESP bar (+y face) and W5500 bar (-y face), front and rear notch
  [(notch_f[0] + notch_f[1])/2, wall + esp_pw + wall, 1, latch_l_in, bump_d_in],
  [(notch_r[0] + notch_r[1])/2, wall + esp_pw + wall, 1, latch_l_in, bump_d_in],
  [(notch_f[0] + notch_f[1])/2, w55_y0 - pclr,        3, latch_l_in, bump_d_in],
  [(notch_r[0] + notch_r[1])/2, w55_y0 - pclr,        3, latch_l_in, bump_d_in]
];
module at_snap(p) translate([p[0], p[1], 0]) rotate([0, 0, 90*p[2]]) children();
z_s0 = rim - skirt_h;   // skirt bottom

// ---------------- Rounding helpers ----------------
// rounded outer outline: concave corners closed with r_inner, convex corners opened with r_vert
module outer2d()
  offset(r = r_vert) offset(delta = -r_vert)
  offset(r = -r_inner) offset(delta = r_inner)
  offset(delta = wall) z_all();

// prism of height h with a rounded edge of radius r at the top (top=true) or bottom,
// built from r_steps offset slices
module rounded_prism(h, r, top = true) {
  translate([0, 0, top ? 0 : r]) linear_extrude(h - r) children();
  for (i = [0 : r_steps - 1]) {
    a1 = 90 * (i + 1) / r_steps;
    z0 = r * sin(90 * i / r_steps);  z1 = r * sin(a1);
    translate([0, 0, top ? h - r + z0 : r - z1])
      linear_extrude(z1 - z0 + 0.01) offset(delta = -r * (1 - cos(a1))) children();
  }
}

// ---------------- Tray ----------------
module tray() {
  difference() {
    union() {
      translate([wall, wall, 0]) {
        difference() {
          union() {
            rounded_prism(z_s0, r_bot, false) outer2d();
            linear_extrude(rim) offset(delta = -(skirt_t + sgap)) outer2d();   // inset under the skirt
          }
          translate([0, 0, floor_t]) linear_extrude(rim) z_all();
        }
        tray_supports();
      }
    }
    rj45_cut();                 // tray: RJ45 only, USB-C cut-out lives in the hood
    // front wall in front of the ESP ends at the skirt bottom; above it the
    // hood skirt alone forms the wall (keeps the USB-C receptacles clear)
    translate([-0.1, wall, z_s0]) cube([wall + 0.11, esp_pw, rim]);
    // button flexures in the floor: U-slot through the floor, tongue thinned from above
    translate([wall, wall, 0]) for (by = btn_ys()) {
      tx0 = esp_x0 + btn_x - tongue_front;
      translate([0, 0, -1]) linear_extrude(floor_t + 1.1) difference() {
        translate([tx0 - tongue_slot, by - tongue_w/2 - tongue_slot]) square([tongue_l + tongue_slot, tongue_w + 2*tongue_slot]);
        translate([tx0, by - tongue_w/2]) square([tongue_l + 0.01, tongue_w]);
      }
      translate([tx0, by - tongue_w/2, tongue_t]) cube([tongue_l, tongue_w, floor_t]);
    }
    // markings: RESET (right seen from the front = lower y), camera icon on BOOT
    translate([wall, wall, -1]) linear_extrude(1 + btn_mark_depth) {
      bx = esp_x0 + btn_x - tongue_front + tongue_l/2;
      translate([bx, btn_ys()[0]]) mirror([0, 1, 0])
        text("RESET", size = btn_label_h / 0.73, font = txt_font, halign = "center", valign = "center");
      translate([bx, btn_ys()[1]]) mirror([0, 1, 0]) camera_icon();
    }
  }
  // plungers on the tongues
  translate([wall, wall, 0]) for (by = btn_ys())
    translate([esp_x0 + btn_x, by, tongue_t - 0.01])
      cylinder(d = plunger_d, h = ze - btn_h - btn_gap - tongue_t + 0.01);
  for (p = snaps()) at_snap(p) latch_bump(p[3], p[4]);
  translate([wall, wall, 0]) esp_front_stops();
}

// camera pictogram from snapshot-camera.svg (24x24 viewBox, stroke width 2,
// round joins), rebuilt natively: OpenSCAD's SVG import ignores strokes.
// Path points converted to y-up and centred on the viewBox centre (12, 12).
camera_h = 5.0;        // overall icon height incl. stroke (tongue is 6 mm wide)
camera_path = [[-8, 5], [-5, 5], [-3, 8], [3, 8], [5, 5], [8, 5], [8.2588, 4.9659], [8.5, 4.866],
  [8.7071, 4.7071], [8.866, 4.5], [8.9659, 4.2588], [9.0, 4.0], [9, -7], [8.9659, -7.2588],
  [8.866, -7.5], [8.7071, -7.7071], [8.5, -7.866], [8.2588, -7.9659], [8.0, -8.0], [-8, -8],
  [-8.2588, -7.9659], [-8.5, -7.866], [-8.7071, -7.7071], [-8.866, -7.5], [-8.9659, -7.2588],
  [-9.0, -7.0], [-9, 4], [-8.9659, 4.2588], [-8.866, 4.5], [-8.7071, 4.7071], [-8.5, 4.866],
  [-8.2588, 4.9659]];
module camera_icon() {
  scale(camera_h / 18) {                       // stroke outline spans 18 units in height
    difference() {                             // body, stroke 2 centred on the path
      offset(r = 1, $fn = 24) polygon(camera_path);
      offset(delta = -1) polygon(camera_path);
    }
    translate([0, -1]) difference() {          // lens: circle r 3.5 at (12, 13)
      circle(r = 4.5, $fn = 64);
      circle(r = 2.5, $fn = 64);
    }
  }
}

function btn_ys() = [esp_y0 + esp_w/2 - btn_dy, esp_y0 + esp_w/2 + btn_dy];

// front stop tabs against the ESP front edge, between and beside the USB-C
// receptacles (in the tray inset behind the hood skirt, outside the openings)
module esp_front_stops() {
  c  = esp_y0 + esp_w/2 + usb_yoff;
  hw = 8.94/2 + usb_stop_margin;                     // receptacle half width + margin
  r_lo = [c - usb_pitch/2 - hw, c - usb_pitch/2 + hw];
  r_hi = [c + usb_pitch/2 - hw, c + usb_pitch/2 + hw];
  x0 = -(wall - skirt_t - sgap);
  for (seg = [[0, r_lo[0]], [r_lo[1], r_hi[0]], [r_hi[1], esp_pw]])
    if (seg[1] - seg[0] > 0.8)
      translate([x0, seg[0], z_s0 - 0.01]) cube([x0 * -1 - esp_stop_gap, seg[1] - seg[0], rim - z_s0 + 0.01]);
}

module tray_supports() {
  for (x = [esp_x0 + esp_pad_front_x, esp_x0 + esp_l - esp_pad], y = [esp_y0, esp_y0 + esp_w - esp_pad])
    translate([x, y, 0]) cube([esp_pad, esp_pad, ze]);
  // ESP rear beam, lip centred (keeps clear of the header strips at the long edges)
  beam_clip(esp_beam_x, 0, esp_pw, rim, clip_t, clip_lip,
            esp_pw/2 - clip_lip_l/2, esp_pw/2 + clip_lip_l/2, esp_beam_gap);
  // rigid end blocks of the beam, standing on the floor: they touch the PCB
  // edge (esp_stop_gap) and act as rear stops; the free beam span between
  // them does the snapping
  for (y = [0, esp_pw - esp_rstop_w])
    translate([esp_x0 + esp_l + esp_stop_gap, y, 0])
      cube([esp_beam_gap - esp_stop_gap + clip_t, esp_rstop_w, rim]);
  echo(str("ESP rear beam: free span ", esp_pw - 2*esp_rstop_w, " mm, strain approx. ",
           round(12000 * clip_t * clip_lip / pow(esp_pw - 2*esp_rstop_w, 2)) / 10, " %"));
  // W5500 rear beam, lip only on the inner half (right, seen from the RJ45);
  // the outer half with the 2x5 header gets no lip
  beam_clip(w55_x0 + w55_l + pclr, w55_y0 - pclr, w55_y0 + w55_w + pclr, w55_top, w55_clip_t, w55_clip_lip,
            w55_y0 + 1, hdr_y0 - 1);
  // W5500 rails; the outer rail stops in front of the 2x5 header (solder joints)
  translate([w55_x0 + 2, w55_y0, 0]) cube([w55_l - 4, 2, zw]);
  translate([w55_x0 + 2, w55_y0 + w55_w - 2, 0]) cube([hdr_x0 - 0.5 - (w55_x0 + 2), 2, zw]);
}

// Beam clip behind a PCB edge (board on the -x side), spanning y0..y1 between
// the pocket walls. The lip (y range ly0..ly1) catches the PCB top face; a lead-in
// ramp on top lets the board push the beam back when pressed in.
module beam_clip(x, y0, y1, z_pcb_top, t, lip, ly0, ly1, reach = 0) {
  span = y1 - y0;
  echo(str("Beam clip span ", span, " mm, lip ", lip, ": strain approx. ",
           round(12000 * t * lip / pow(span, 2)) / 10, " %"));
  translate([x, y0, z_pcb_top - 1.0]) cube([t, span, clip_h]);
  // ends anchored in the side walls (only below the rim, the hood sits above)
  for (ya = [y0 - 0.5, y1]) translate([x, ya, z_pcb_top - 1.0]) cube([t, 0.5, min(clip_h, rim - (z_pcb_top - 1.0))]);
  translate([x, ly0, z_pcb_top + 0.1]) hull() {
    translate([-lip - reach, 0, 0]) cube([lip + reach + t, ly1 - ly0, 0.4]);
    translate([0, 0, clip_h - 1.2]) cube([t, ly1 - ly0, 0.1]);
  }
}

// local frame for latches: +x = outward, outer face at x = 0
// bump on the tray inset face: flat catch face at the bottom, lead-in ramp on top
module latch_bump(L, bd) {
  x_in = -(skirt_t + sgap);
  zb = z_s0 + beam_h + 0.15;
  bl = L - 6;
  echo(str("Latch L=", L, " mm: beam strain approx. ", round(12000 * skirt_t * (bd - sgap) / pow(L, 2)) / 10, " %"));
  translate([0, -bl/2, 0]) hull() {
    translate([x_in - 0.01, 0, zb]) cube([bd + 0.01, bl, 0.6]);
    translate([x_in - 0.01, 0, zb + bump_zh - 0.01]) cube([0.01, bl, 0.01]);
  }
}

// window in the skirt above the latch beam
module latch_window(L) {
  translate([-skirt_t - 0.1, -L/2, z_s0 + beam_h]) cube([skirt_t + 0.2, L, win_h]);
}

// ---------------- Hood ----------------
h_top = max(h_esp, h_rj45, h_hdr) + hood_extra;   // common bar height
zones = [ [0, h_top], [1, h_top], [2, h_top] ];
module zone(i) {
  if (i == 0) z_esp(); else if (i == 1) z_w55bar(); else z_bridge();
}

module hood() {
  difference() {
    union() {
      translate([wall, wall, rim]) rounded_prism(h_top - rim + roof_t, r_top, true) outer2d();
      // skirt overlapping the tray wall
      translate([wall, wall, z_s0]) linear_extrude(skirt_h + 0.01) difference() {
        outer2d();
        offset(delta = -skirt_t) outer2d();
      }
    }
    for (p = snaps()) at_snap(p) latch_window(p[3]);
    translate([wall, wall, 0]) for (z = zones)
      translate([0, 0, rim - 1]) linear_extrude(z[1] - rim + 1) zone(z[0]);
    common_cuts();
    // port labels engraved into the front face
    for (pl = port_labels) port_label(pl[0], pl[1]);
    // debossed text - ESP roof: product name; W5500 roof: network defaults
    translate([wall + esp_pl/2, wall + esp_pw/2, h_top + roof_t - txt_depth])
      linear_extrude(txt_depth + 1) fit_text(txt1, txt1_h, 78.5, txt_maxw);
    translate([wall + ix/2, wall + w55_y0 + w55_w/2, h_top + roof_t - txt_depth])
      linear_extrude(txt_depth + 1) {
        // both lines in the same size, set by the longer second line
        translate([0,  4.5]) fit_text(txt2a, txt2_h, 99.3, txt_maxw);
        translate([0, -4.5]) fit_text(txt2b, txt2_h, 99.3, txt_maxw);
      }
  }
  // hold-down finger on the ESP centre line above the buttons (button force pushes up)
  translate([wall + esp_x0 + btn_x, wall + esp_y0 + esp_w/2, rim]) cylinder(d = finger_d + 1, h = h_top - rim + 0.5);
  // hold-down fingers on the W5500 corners
  // (rear corners: header on the outer side, snap clip on the inner side)
  for (p = [[w55_x0 + 2.5, w55_y0 + 2.5], [w55_x0 + 2.5, w55_y0 + w55_w - 2.5]]) let(x = p[0], y = p[1])
    translate([wall + x, wall + y, w55_top]) cylinder(d = finger_d, h = h_top - w55_top + 0.5);
}

// text in the front face plane: reads left-to-right seen from the front
// (right = -y, up = +z), cut port_label_depth deep into the face at x = 0
module port_label(t, side) {
  yc = wall + esp_y0 + esp_w/2 + usb_yoff + side * usb_pitch/2;
  zc = usb_zc + usb_open_h/2 + port_label_gap + port_label_h/2;
  multmatrix([[0, 0, -1, port_label_depth], [-1, 0, 0, yc], [0, 1, 0, zc], [0, 0, 0, 1]])
    linear_extrude(port_label_depth + 1)
      text(t, size = port_label_h / 0.73, font = txt_font, halign = "center", valign = "center");
}

module fit_text(t, h, w10, maxw) {
  text(t, size = min(h / 0.73, maxw * 10 / w10), font = txt_font, halign = "center", valign = "center");
}

// cut-outs shared by tray and hood
module common_cuts() {
  // 2x USB-C front (two stadium holes, MagSafe case geometry)
  translate([-1, wall + esp_y0 + esp_w/2 + usb_yoff, usb_zc])
    rotate([0, 90, 0]) linear_extrude(wall + 2)
      for (dy = [-usb_pitch/2, usb_pitch/2])
        translate([0, dy]) hull() for (e = [-1, 1])
          translate([0, e * (usb_open_w - usb_open_h)/2]) circle(d = usb_open_h, $fn = 48);
  // RJ45 front
  translate([-1, wall + w55_y0 + w55_w/2 + rj45_yoff - (rj45_w + 0.8)/2, w55_top - 0.2])
    cube([wall + 2, rj45_w + 0.8, rj45_h + 0.5]);
}
module rj45_cut() {
  // RJ45 front
  translate([-1, wall + w55_y0 + w55_w/2 + rj45_yoff - (rj45_w + 0.8)/2, w55_top - 0.2])
    cube([wall + 2, rj45_w + 0.8, rj45_h + 0.5]);
}


// ---------------- Dummy boards ----------------
module dummies() {
  color("seagreen", 0.85) translate([wall + esp_x0, wall + esp_y0, ze]) {
    cube([esp_l, esp_w, esp_t]);
    translate([esp_l - 25.5, (esp_w - mod_w)/2, -3.2]) cube([25.5, mod_w, 3.2]);
    for (dy = [-usb_pitch/2, usb_pitch/2])
      translate([-usb_proj, esp_w/2 + usb_yoff + dy - 4.47, -usb_body_h]) cube([7.5, 8.94, usb_body_h]);
    for (y = [0, esp_w - 2.54]) translate([6, y, esp_t]) cube([40.64, 2.54, esp_pin_h]);
  }
  %translate([wall + esp_x0 + esp_l - 7, wall + ant_edge - mod_w - rf_clear, ze - rf_clear])
    cube([7 + rf_clear, mod_w + 2*rf_clear, 2*rf_clear]);
  color("royalblue", 0.85) translate([wall + w55_x0, wall + w55_y0, zw]) {
    cube([w55_l, w55_w, w55_t]);
    translate([0, w55_w/2 + rj45_yoff - rj45_w/2, w55_t]) cube([rj45_l, rj45_w, rj45_h]);
    translate([hdr_x0 - w55_x0, hdr_y0 - w55_y0, w55_t]) cube([hdr_lx, hdr_ly, w55_pin_h]);
  }
}

// ---------------- Output ----------------
if (part == "tray") tray();
else if (part == "hood") hood();      // print upright (roofs are short bridges)
else {
  tray();
  dummies();
  translate([0, 0, 15]) hood();
}

echo(str("Outer size: ", ox, " x ", oy, " x ", h_top + roof_t, " mm"));
echo(str("Bar height ", h_top + roof_t, " mm, bridge ", h_top + roof_t, " mm, bar length ", ix + 2*wall, " mm"));
echo(str("Antenna area -> W5500 edge: ", w55_y0 - ant_edge, " mm"));
echo(str("Antenna area -> wire bridge (x): ", (esp_x0 + esp_l - 7) - (bridge_x0 + bridge_l), " mm"));
