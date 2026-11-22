// IM73D122 USB-A mic (v3.3 board + adapter) - tube case
// A rounded-rectangle USB end lofts into a round mic end.
// The board slides in from the round end, plug first, until the board's
// front edge meets the ledge round the plug hole and the plug sticks out
// of the rectangular hole. At the round end the bore is the adapter's
// rectangle plus a snug circle for the 15.5 mm module: the module sits in
// the circle (loosely), and a ridge just behind it stops the module going
// further in.
//
// Coordinates: Z runs along the tube (Z = 0 is the outside of the USB
// end, the face it prints on). X is across the board, Y is up from the
// board. The round end is centred on the mic (Y = 0); the board's top
// surface is at Y = -MIC_Z.
//
// All sizes are in mm. Measure your parts and adjust the values below.

$fn = 96;

// ---- board stack (from the KiCad v3.3 design) ---------------------------
MIC_Z      = 1.24;   // mic centre above the USB board's top surface
BOARD_L    = 18.5;   // USB board length, front edge to back edge
BOARD_W    = 14.0;   // USB board width
ADP_T      = 1.6;    // adapter board thickness
ADP_W      = 13.97;  // adapter width
ADP_TOP    = 6.05;   // adapter top edge above the mic centre
ADP_BOT    = 6.65;   // adapter bottom edge below the mic centre
ADP_CR     = 2.54;   // adapter corner radius
MOD_D      = 15.5;   // IM73D122 module diameter
MOD_T      = 1.0;    // module board thickness
MOD_FRONT  = 5.1;    // module front face, measured from the adapter's back face
PLUG_W     = 12.0;   // USB-A shell width
PLUG_H     = 4.5;    // USB-A shell height (rests on the board's top surface)
BOT_PARTS  = 3.4;    // below the board top: board + U1 (J2 is behind the loft)

// ---- case ------------------------------------------------------------------
CLR        = 0.2;    // fit clearance for the boards
MOD_CLR    = 0.3;    // clearance round the module (loose: the module may sit off-centre on the adapter)
RIDGE_LAP  = 0.6;    // how far the ridge reaches in under the module's edge
WALL       = 1.4;    // minimum wall thickness
FRONT_T    = 1.5;    // USB-end wall thickness
LOFT_END   = 14;     // where the loft reaches the round section, from the board's front edge
RIDGE_T    = 1.2;    // ridge length behind the module (where the module stops)
HOOD       = 1.0;    // tube length past the module face, to protect the mic

// derived
Z0        = FRONT_T;                          // board front edge
ADP_BACK  = Z0 + BOARD_L;                     // adapter back face
MOD_BACK  = ADP_BACK + MOD_FRONT - MOD_T;     // module back face
CASE_L    = ADP_BACK + MOD_FRONT + HOOD;
MOD_R     = MOD_D / 2 + MOD_CLR;
AW        = ADP_W / 2 + CLR;
// farthest point of the adapter's rounded corners from the mic axis, plus the wall
OUT_R     = sqrt(pow(AW - ADP_CR - CLR, 2) + pow(ADP_BOT - ADP_CR, 2)) + ADP_CR + CLR + WALL;

// cross-sections (2D, X across, Y up, origin = mic axis)
module rrect(x0, x1, y0, y1, r) {
    translate([x0 + r, y0 + r]) offset(r) square([x1 - x0 - 2*r, y1 - y0 - 2*r]);
}
module inner_front() {           // the plug slides the whole length: clear board, U1 and the plug shell
    w = BOARD_W/2 + CLR + 0.1;
    rrect(-w, w, -MIC_Z - BOT_PARTS - CLR, -MIC_Z + PLUG_H + CLR, 1.0);
}
module adapter_hole() {          // adapter outline + clearance (also clears J2, board and plug)
    rrect(-AW, AW, -ADP_BOT - CLR, ADP_TOP + CLR, ADP_CR + CLR);
}
module plug_hole() {
    w = PLUG_W/2 + CLR;
    rrect(-w, w, -MIC_Z - CLR, -MIC_Z + PLUG_H + CLR, 0.6);
}
module outer_front() {
    w = BOARD_W/2 + CLR + 0.1 + WALL;
    rrect(-w, w, -MIC_Z - BOT_PARTS - CLR - WALL, -MIC_Z + PLUG_H + CLR + WALL, 2.5);
}
module slab(z, t = 0.01) { translate([0, 0, z]) linear_extrude(t) children(); }

module case() {
    difference() {
        union() {
            hull() { slab(0) outer_front(); slab(Z0 + LOFT_END) circle(OUT_R); }
            translate([0, 0, Z0 + LOFT_END]) cylinder(r = OUT_R, h = CASE_L - Z0 - LOFT_END);
        }
        // inner loft from the USB end to the adapter's outline
        hull() { slab(Z0) inner_front(); slab(Z0 + LOFT_END) adapter_hole(); }
        // adapter's outline the rest of the way (the ridge is the part of
        // this length where the module's circle is not cut)
        translate([0, 0, Z0 + LOFT_END - 0.01]) linear_extrude(CASE_L) adapter_hole();
        // snug circle for the module, from the ridge to the open end
        translate([0, 0, MOD_BACK - 0.2]) cylinder(r = MOD_R, h = CASE_L);
        // ridge opening: the adapter's outline plus a circle a little smaller than the module
        translate([0, 0, Z0 + LOFT_END]) cylinder(r = MOD_D / 2 - RIDGE_LAP, h = CASE_L);
        // plug hole through the front wall
        translate([0, 0, -1]) linear_extrude(FRONT_T + 2) plug_hole();
    }
}

case();
