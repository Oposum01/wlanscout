// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

// =====================================================================
//  Enclosure v21 - form-fitting, soldered wiring, connectors at the front,
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
hood_extra = 6.0;    // extra headroom inside the hood (cable connectors)
// edge rounding
r_vert   = 3.0;     // vertical outer corners (plan view)
r_inner  = 2.0;     // concave corners where the bridge meets the bars
r_top    = 2.0;     // top edge of the hood
r_bot    = 1.2;     // bottom edge of the tray
r_steps  = 8;       // slices per rounded edge
pclr    = 0.3;       // pocket clearance around each PCB
rf_clear = 15.0;     // antenna area -> W5500 copper (Espressif >= 15 mm)
bridge_l = 16.0;     // width of the wire bridge between the boards (wires stay >= 15 mm from the antenna)

// ---------------- ESP32-C5 Waveshare (measured by user) ----------------
esp_l = 61.5;  esp_w = 25.6;  esp_t = 1.6;   // antenna ends flush with the rear PCB edge
esp_comp_h = 3.3;           // tallest part on the component side
pad_h      = 5.0;           // carrier board above floor
esp_pad = 2.5;  esp_pad_front_x = 8.5;   // front pads behind the USB-C (check buttons)
esp_pin_h  = 8.5;           // header plastic + pin above PCB (clip pins to lower)
wire_sp    = 2.5;           // solder joint + wire bend above pin tips
mod_w = 18.0;
usb_pitch = 13.0;  usb_yoff = 0.0;       // MEASURE (board is flipped)
usb_lower = 2.0;    // USB-C opening extended below the skirt bottom (into the tray front wall)
usb_plug_w = 12.5;  usb_h = 7.5;  usb_proj = 0.8;  usb_body_h = 3.26;
// Rear retention: horizontal beam behind the PCB edge, spanning the full pocket
// width and anchored in both side walls (fixed-fixed). It flexes in the layer
// plane like the hood latches; strain ~12*t*lip/span^2.
clip_t = 1.2;  clip_lip = 0.8;  clip_lip_l = 14.0;      // ESP: lip centred on the beam
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
txt1 = "wlanscout+";  txt2a = "5x DHCP or Static:";  txt2b = "192.168.1.1";
txt1_h = 6.5;  txt2_h = 4.0;
txt_font = "DejaVu Sans Condensed:style=Bold";
txt_depth = 0.8;

$fn = 40;

// ---------------- Derived layout (interior coordinates) ----------------
esp_x0 = 0.3;                                  // USB-C reaches into the front wall
esp_y0 = pclr;
ze     = floor_t + pad_h;                      // ESP carrier, bottom face
rim    = ze + esp_t;                           // tray rim = ESP top (flush)
esp_pl = esp_x0 + esp_l + 0.2 + clip_t + clip_lip + 0.3;  // ESP pocket length incl. clip beam + flex room
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
    // USB-C opening reaches usb_lower further down into the tray front wall
    translate([-0.1, wall + esp_y0 + esp_w/2 + usb_yoff - (usb_pitch + usb_plug_w)/2, z_s0 - usb_lower])
      cube([wall + 0.11, usb_pitch + usb_plug_w, usb_lower + 0.01]);
  }
  for (p = snaps()) at_snap(p) latch_bump(p[3], p[4]);
}

module tray_supports() {
  for (x = [esp_x0 + esp_pad_front_x, esp_x0 + esp_l - esp_pad], y = [esp_y0, esp_y0 + esp_w - esp_pad])
    translate([x, y, 0]) cube([esp_pad, esp_pad, ze]);
  // ESP rear beam, lip centred (keeps clear of the header strips at the long edges)
  beam_clip(esp_x0 + esp_l + 0.2, 0, esp_pw, rim, clip_t, clip_lip,
            esp_pw/2 - clip_lip_l/2, esp_pw/2 + clip_lip_l/2);
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
module beam_clip(x, y0, y1, z_pcb_top, t, lip, ly0, ly1) {
  span = y1 - y0;
  echo(str("Beam clip span ", span, " mm, lip ", lip, ": strain approx. ",
           round(12000 * t * lip / pow(span, 2)) / 10, " %"));
  translate([x, y0, z_pcb_top - 1.0]) cube([t, span, clip_h]);
  // ends anchored in the side walls (only below the rim, the hood sits above)
  for (ya = [y0 - 0.5, y1]) translate([x, ya, z_pcb_top - 1.0]) cube([t, 0.5, min(clip_h, rim - (z_pcb_top - 1.0))]);
  translate([x, ly0, z_pcb_top + 0.1]) hull() {
    translate([-lip, 0, 0]) cube([lip + t, ly1 - ly0, 0.4]);
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
    // debossed text on the ESP roof
    translate([wall + esp_pl/2, wall + esp_pw/2, h_top + roof_t - txt_depth])
      linear_extrude(txt_depth + 1) {
        maxw = esp_pl - 4;
        translate([0,  6.5]) fit_text(txt1,  txt1_h, 78.5, maxw);
        translate([0, -1.0]) fit_text(txt2a, txt2_h, 124.5, maxw);
        translate([0, -7.0]) fit_text(txt2b, txt2_h, 124.5, maxw);
      }
  }
  // hold-down fingers on the W5500 corners
  // (rear corners: header on the outer side, snap clip on the inner side)
  for (p = [[w55_x0 + 2.5, w55_y0 + 2.5], [w55_x0 + 2.5, w55_y0 + w55_w - 2.5]]) let(x = p[0], y = p[1])
    translate([wall + x, wall + y, w55_top]) cylinder(d = finger_d, h = h_top - w55_top + 0.5);
}

module fit_text(t, h, w10, maxw) {
  text(t, size = min(h / 0.73, maxw * 10 / w10), font = txt_font, halign = "center", valign = "center");
}

// cut-outs shared by tray and hood
module common_cuts() {
  // 2x USB-C front (one slot)
  translate([-1, wall + esp_y0 + esp_w/2 + usb_yoff, usb_zc])
    rotate([0, 90, 0]) linear_extrude(wall + 2)
      hull() for (dy = [-usb_pitch/2, usb_pitch/2])
        translate([0, dy]) offset(r = 2.5) square([usb_h - 5, usb_plug_w - 5], center = true);
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
